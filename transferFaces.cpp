/*
 * Ugly hack to transfer the faces marked in Aperture to Lightroom (V6.x)
 * by Daniel Höpfl <daniel@hoepfl.de>
 * 
 * Compile using:
 * clang++ -std=c++11 -o transferFaces transferFaces.cpp -lsqlite3 -framework CoreFoundation -lstdc++ `xml2-config --cflags --libs`
 *
 * Call it:
 * ./transferFaces -l <D&D lightroom catalog> \
 *                 -a <D&D aperture bundle> \
 *                 -k "Name of folder that contains all Aperture Face Keywords"
 *
 * Typical call:
 * ./transferFaces -l "Lightroom Catalog.lrcat" -a "Aperture Library.aplibrary" -k "Faces from Aperture"
 */

#include <iostream>
#include <cstdlib>
#include <sstream>
#include <cmath>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <sqlite3.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <sys/stat.h>
#include <CoreFoundation/CFString.h>
#include "tf_sql.hpp"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>


/// struct to store the data of a face
typedef struct
{
   // The coordinates as stored by Aperture
   double bl_x;
   double bl_y;
   double br_x;
   double br_y;
   double tl_x;
   double tl_y;
   double tr_x;
   double tr_y;

   // The name of the person
   std::string name;
} facedata;

std::string g_keywordsRoot = "Faces from Aperture";
std::string g_tagKeywordsRoot = "Tags from Aperture";

/**
 * Normalize a UTF-8 encoded string to use composed character form.
 *
 * @param str  The UTF-8 encoded string.
 * @return The same string but all characters in the composed form.
 */
std::string normalizeUTF8(const std::string &str)
{
   CFMutableStringRef mut = CFStringCreateMutable(NULL, 0);
   CFStringAppendCString(mut, str.c_str(), kCFStringEncodingUTF8);
   CFStringNormalize(mut, kCFStringNormalizationFormC);

   CFIndex size = str.size()*4;
   char *buffer = (char *) malloc(size);
   CFStringGetCString(mut, buffer, size, kCFStringEncodingUTF8);
   std::string result(buffer);

   free(buffer);
   CFRelease(mut);

   return result;
}

/**
 * Aperture stores all keywords as UTF-8, using the decomposed form of
 * characters ("ö", U+00F6, is stored as "o", U+006F, plus "COMBINING
 * DIAERESIS", U+0308). Since both are a valid forms to store texts, Lightroom
 * handles these keywords just fine.
 *
 * Windows, on the other hand, tends to fail rendering these characters.
 * Therefore I decided to convert all keywords to UTF-8 into the composed form.
 *
 * While this might change the Unicode codepoints used, it does not change
 * the way the keyword looks or what the characters mean.
 *
 * The second advantage of converting all keywords to a normalized form is that
 * it makes searching much easier (since SQLite does a simple byte-by-byte
 * comparison, ignoring cononical equivalence).
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @return @c true on succes, @c false on any error.
 */
bool fixKeywordsUTF8(::sqlite3 *lightroomDB)
{
   TFSql sql(lightroomDB,
             "SELECT id_local, lc_name, name "
             "FROM AgLibraryKeyword");
   while (sql.step()) {
      ::sqlite3_int64 id_local = sql.column_int64(0);
      std::string lc_name = sql.column_str(1);
      std::string name = sql.column_str(2);

      if (name != "") {
         std::cout << "Normalizing keyword \"" << name << "\"" << std::endl;
      }

      TFSql update(lightroomDB,
                   "UPDATE AgLibraryKeyword "
                   "SET lc_name = ?, "
                   "    name = ? "
                   "WHERE id_local = ?");
      if (lc_name == "") {
         update.bind(1);
      } else {
         update.bind(1, normalizeUTF8(lc_name));
      }
      if (name == "") {
         update.bind(2);
      } else {
         update.bind(2, normalizeUTF8(name));
      }
      update.bind(3, id_local);
      update.step();

      if (update.hasFailed()) {
         std::cerr << "Failed to update keyword to be in composed form: " << update.getErrorMsg() << std::endl;
         return false;
      }
   }

   if (sql.hasFailed()) {
      std::cerr << "Failed to update keywords to be in composed form: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

/**
 * Helper to store a sqlite_int64 into a variable.
 *
 * @param voidptr Pointer to the sqlite_int64 to store the value into.
 * @param columnCount Number of columns the sqlite3_exec call returned.
 * @param columnTexts The textual representation of the columns.
 * @param columnNames The names of the columns.
 * @return 0.
 */
int storeInt64(void *voidptr, int columnCount, char **columnTexts, char **columnNames)
{
   ::sqlite3_int64 *int64ptr = (::sqlite3_int64 *) voidptr;
   *int64ptr = ::strtoll(columnTexts[0], NULL, 10);
   return 0;
}

/**
 * Lightroom does not use IDs created by SQLite has a central ID counter that it
 * uses to find unique IDs.
 *
 * This method reads the current value, increments it by one and stores the new
 * value.
 * 
 * IMPROVE ME: Since we are the only one that writes to the database during face
 * transfer, we could handle this in memory and store the last value on exit.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @return The next usable ID.
 */
::sqlite3_int64 getNextLocalID(::sqlite3 *lightroomDB) {
   char *errorMsg = NULL;
   ::sqlite3_int64 id_local = -1;
   if (SQLITE_OK != ::sqlite3_exec(lightroomDB,
                                   "SELECT value "
                                   "FROM Adobe_variablesTable "
                                   "WHERE name = 'Adobe_entityIDCounter'",
                                   storeInt64,
                                   (void *) &id_local,
                                   &errorMsg) || id_local < 0) {
      std::cerr << "Failed to get next id_local: " << errorMsg << std::endl;
      sqlite3_free(errorMsg);
      return -1;
   }
   if (SQLITE_OK != ::sqlite3_exec(lightroomDB,
                                   "UPDATE Adobe_variablesTable "
                                   "SET value = value + 1 "
                                   "WHERE name = 'Adobe_entityIDCounter'",
                                   NULL,
                                   NULL,
                                   &errorMsg)) {
      std::cerr << "Failed to get next id_local: " << errorMsg << std::endl;
      sqlite3_free(errorMsg);
      return -1;
   }

   return id_local;
}

/** The Aperture importer of Lightroom creates a keyword for each person it
 * imports. It does not mark these as "persons" as it is done for keywords that
 * are created by Lightroom when you enter a new name for a detected face.
 *
 * This method changes all keywords that are childs of a given keyword (hint:
 * "Faces from Aperture") to be marked as "person".
 *
 * NOTE: Failure is ignored.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param faces_id      The ID of the keyword that is the parent of all keywords
 *                      that should be marked as persons.
 */
void fixApertureFaceTagToBePersons(::sqlite3 *lightroomDB, ::sqlite3_int64 faces_id)
{
   TFSql sql(lightroomDB,
             "UPDATE AgLibraryKeyword "
             "SET keywordType = 'person' "
             "WHERE keywordType IS NULL "
             "AND parent = ?");
   sql.bind(1, faces_id);
   sql.step();

   if (sql.hasFailed()) {
      std::cerr << "Failed to fix face tags of aperture to be of type person: " << sql.getErrorMsg() << std::endl;
   }
}

// TODO: Docu
::sqlite3_int64 getTagRootKeywordId(::sqlite3 *lightroomDB)
{
   static ::sqlite3_int64 g_tags_keywords_root_id = -1;

   if (g_tags_keywords_root_id >= 0) {
      return g_tags_keywords_root_id;
   }

   ::sqlite3_int64 root_id = -1;
   if (g_tagKeywordsRoot != "") {
      TFSql sql(lightroomDB,
                "SELECT id_local "
                "FROM AgLibraryKeyword "
                "WHERE name = ?");
      sql.bind(1, g_tagKeywordsRoot.c_str());
      if (sql.step()) {
         root_id = sql.column_int64(0);
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to find tag keywords root: " << sql.getErrorMsg() << std::endl;
         return -1;
      }
   } else {
      char *errorMsg = NULL;
      if (SQLITE_OK != ::sqlite3_exec(lightroomDB,
                                      "SELECT value "
                                      "FROM Adobe_variablesTable "
                                      "WHERE name = 'AgLibraryKeyword_rootTagID'",
                                      storeInt64,
                                      (void *) &root_id,
                                      &errorMsg) || root_id < 0) {
         std::cerr << "Failed to find keywords root: " << errorMsg << std::endl;
         sqlite3_free(errorMsg);
         return -1;
      }
   }

   g_tags_keywords_root_id = root_id;

   return root_id;
}


/**
 * Finds the ID of the keyword all imported person name keywords should be a child of.
 * The name of this keyword is configured via command line argument. If the keyword is given as empty string, the root folder is used.
 *
 * IMPROVE ME: As an optimisation (unnecessarily, as it turned out), we store the ID and search only once for it.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @return The ID to place face keywords under.
 */
::sqlite3_int64 getRootKeywordId(::sqlite3 *lightroomDB)
{
   static ::sqlite3_int64 g_keywords_root_id = -1;

   if (g_keywords_root_id >= 0) {
      return g_keywords_root_id;
   }

   ::sqlite3_int64 root_id = -1;
   if (g_keywordsRoot != "") {
      TFSql sql(lightroomDB,
                "SELECT id_local "
                "FROM AgLibraryKeyword "
                "WHERE name = ?");
      sql.bind(1, g_keywordsRoot.c_str());
      if (sql.step()) {
         root_id = sql.column_int64(0);
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to find keywords root: " << sql.getErrorMsg() << std::endl;
         return -1;
      }

      fixApertureFaceTagToBePersons(lightroomDB, root_id);
   } else {
      char *errorMsg = NULL;
      if (SQLITE_OK != ::sqlite3_exec(lightroomDB,
                                      "SELECT value "
                                      "FROM Adobe_variablesTable "
                                      "WHERE name = 'AgLibraryKeyword_rootTagID'",
                                      storeInt64,
                                      (void *) &root_id,
                                      &errorMsg) || root_id < 0) {
         std::cerr << "Failed to find keywords root: " << errorMsg << std::endl;
         sqlite3_free(errorMsg);
         return -1;
      }
   }

   g_keywords_root_id = root_id;

   return root_id;
}

/**
 * Lightroom uses a database column genealogy that reflects the tree of keywords. This method finds the genealogy string of the face keyword root.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @return The genealogy string of the faces root keyword (or "" on error).
 */
std::string getRootKeywordGenealogy(::sqlite3 *lightroomDB)
{
   static std::string g_keywords_root_genealogy;

   if (g_keywords_root_genealogy != "") {
      return g_keywords_root_genealogy;
   }

   ::sqlite3_int64 root_id = getRootKeywordId(lightroomDB);
   if (root_id == -1) {
      return "";
   }

   TFSql sql(lightroomDB,
             "SELECT genealogy FROM AgLibraryKeyword "
             "WHERE id_local = ?");
   sql.bind(1, root_id);
   if (!sql.step() || sql.hasFailed()) {
      std::cerr << "Failed to select root genealogy: " << sql.getErrorMsg() << std::endl;
      return "";
   }
   g_keywords_root_genealogy = sql.column_str(0);
   return g_keywords_root_genealogy;
}

/**
 * Finds the UUID of the master image in the Aperture database based on the
 * image filename and the image date.
 * 
 * @param apertureDB    The handle of the Aperture database.
 * @param fileName      The filename of the image to search.
 * @param imageDate     The date the image was taken (in Aperture's semantic, Mac epoch!).
 * @return The UUID (or "" on error)
 */
std::string findImageUUIDForFilename(::sqlite3 *apertureDB,
                                     const std::string &fileName,
                                     ::sqlite3_int64 imageDate)
{
   TFSql sql(apertureDB,
             "SELECT uuid, isMissing "
             "FROM RKMaster "
             "WHERE fileName = ? "
             "AND fileModificationDate = ? "
             "GROUP BY imagePath "
             "ORDER BY isMissing");
   sql.bind(1, fileName);
   sql.bind(2, imageDate);
   std::string masterUUID;
   if (sql.step()) {
      masterUUID = sql.column_str(0);

      if (sql.step()) {
         if (sql.column_int64(1) == 0) {
            std::cerr << "Warning: More than one UUID for filename " << fileName << ", date " << imageDate << std::endl;
         }
      }
   } else if (!sql.hasFailed()) {
      std::cerr << "Warning: Did not find UUID for image list statement of file " << fileName << ", " << imageDate << " ";

      sql.reset("SELECT uuid "
                "FROM RKMaster "
                "WHERE fileModificationDate = ? ");
      sql.bind(1, imageDate);
      if (sql.step()) {
         masterUUID = sql.column_str(0);
         if (sql.step()) {
            std::cerr << std::endl;
            std::cerr << "Error: Searching for UUID for image list statement of file " << fileName << ", " << imageDate << " was not unique when searching for file creation time only";
            masterUUID = "";
         } else {
            std::cerr << "but found by creation date.";
         }
      } else {
         std::cerr << std::endl;
         std::cerr << "Error: Searching for UUID for image list statement of file " << fileName << ", " << imageDate << " did not find UUID";
      }

      std::cerr << std::endl;
   }

   if (sql.hasFailed()) {
      std::cerr << "Failed to read UUID for image list statement of file " << fileName << ", " << imageDate<< ": " << sql.getErrorMsg() << std::endl;
      return "";
   }
   return masterUUID;
}

/**
 * Finds all face data stored in Aperture's database for a given image.
 *
 * @param apertureDB    The handle of the Aperture database.
 * @param facesDB       The handle of the face DB of Aperture.
 * @param fileName      The filename of the image to search.
 * @param imageDate     The date the image was taken (in Aperture's semantic, Mac epoch!).
 * @return A list of facedata structs.
 */
std::deque<facedata> findFacesForImage(::sqlite3 *apertureDB,
                                      ::sqlite3 *facesDB,
                                      const std::string &fileName,
                                      ::sqlite3_int64 imageDate)
{
   std::deque<facedata> result;

   std::string masterUUID = findImageUUIDForFilename(apertureDB, fileName, imageDate);
   if (masterUUID != "") {
      TFSql sql(facesDB,
                "SELECT bottomLeftX, bottomLeftY, bottomRightX, bottomRightY, topLeftX, topLeftY, topRightX, topRightY, faceKey "
                "FROM RKDetectedFace "
                "WHERE masterUuid = ? "
                "AND rejected = 0 ");
      sql.bind(1, masterUUID);
      while (sql.step()) {
         facedata fd;
         fd.bl_x = sql.column_double(0);
         fd.bl_y = sql.column_double(1);
         fd.br_x = sql.column_double(2);
         fd.br_y = sql.column_double(3);
         fd.tl_x = sql.column_double(4);
         fd.tl_y = sql.column_double(5);
         fd.tr_x = sql.column_double(6);
         fd.tr_y = sql.column_double(7);
         fd.name = "";

         ::sqlite_int64 faceKey = sql.column_double(8);

         if (!sql.hasFailed()) {
            TFSql faceNameSql(facesDB,
                      "SELECT name "
                      "FROM RKFaceName "
                      "WHERE faceKey = ?");
            faceNameSql.bind(1, faceKey);
            if (faceNameSql.step()) {
               fd.name = normalizeUTF8(faceNameSql.column_str(0));
            }

            if (!faceNameSql.hasFailed()) {
               result.push_back(fd);
            } else {
               std::cerr << "Failed to get name of face: " << faceNameSql.getErrorMsg() << std::endl;
            }
         }
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to list faces: " << sql.getErrorMsg() << std::endl;
         return result;
      }
   }

   return result;
}

/**
 * Searches for a keyword with a given name. This keyword has to be under the
 * faces root keyword and it has to be of the "person" type (Last condition is
 * fixed for all keywords created by the Aperture importer, see
 * fixApertureFaceTagToBePersons()).
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param name          The name of the person/keyword to find.
 * @return The ID of the keyword or -1 if no such keyword was found.
 */
::sqlite3_int64 findExistingKeywordID(::sqlite3 *lightroomDB, std::string &name)
{
   TFSql sql(lightroomDB,
             "SELECT id_local "
             "FROM AgLibraryKeyword "
             "WHERE genealogy LIKE ? "
             "AND name is ? "
             "AND keywordType = 'person'");
   sql.bind(1, getRootKeywordGenealogy(lightroomDB) + "%");
   sql.bind(2, name);

   ::sqlite3_int64 existing_id = -1;
   if (!sql.step()) {
      if (sql.hasFailed()) {
         std::cerr << "Failed to read existing keyword: " << sql.getErrorMsg() << std::endl;
         return -1;
      }
   } else {
      existing_id = sql.column_int64(0);
   }

   return existing_id;
}

/**
 * Helper that creates a new random UUID.
 *
 * @return Newly created UUID.
 */
std::string uuid(void)
{
   ::uuid_t uuidStruct;
   ::uuid_generate_random(uuidStruct);
   char s[37];
   ::uuid_unparse(uuidStruct, s);

   return s;
}

/**
 * Creates a new person keyword. The created keyword is a direct child of the faces root keyword.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param name          The name of the keyword.
 * @return The ID used for the new keyword.
 */
::sqlite3_int64 createNewKeyword(::sqlite3 *lightroomDB, const std::string &name, ::sqlite3_int64 root_id, const char *type)
{
   if (root_id == -1) {
      return -1;
   }

   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return -1;
   }

   TFSql sql(lightroomDB,
             "INSERT into AgLibraryKeyword(id_local, id_global, dateCreated, imageCountCache, keywordType, lastApplied, lc_name, name, parent) "
             "VALUES(?, ?, "
             "       (julianday('now') - 2440587.5)*86400.0 - strftime('%s','2001-01-01 00:00:00'), "
             "       NULL, "
             "       ?, "
             "       (julianday('now') - 2440587.5)*86400.0 - strftime('%s','2001-01-01 00:00:00'), "
             "       lower(?), "
             "       ?, "
             "       ?)");
   sql.bind(1, id_local);
   sql.bind(2, uuid());
   if (type) {
      sql.bind(3, std::string(type));
   } else {
      sql.bind(3);
   }
   sql.bind(4, name);
   sql.bind(5, name);
   sql.bind(6, root_id);
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert keyword: " << sql.getErrorMsg() << std::endl;
      return -1;
   }

   // Build genealogy string
   std::stringstream genealogy_stream;
   genealogy_stream << getRootKeywordGenealogy(lightroomDB) << "/" << int(::log10(id_local)+1) << id_local;
   std::string genealogy = genealogy_stream.str();

   sql.reset("UPDATE AgLibraryKeyword "
             "SET genealogy = ? "
             "WHERE id_local = ?");
   sql.bind(1, genealogy);
   sql.bind(2, id_local);
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to set genealogy of keyword: " << sql.getErrorMsg() << std::endl;
      return -1;
   }

   return id_local;
}

/**
 * Creates a new row in the AgLibraryFaceCluster table.
 *
 * IMPROVE ME: I have no idea what this table is good for. But Lightroom does
 * create entrys here, so do I.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @return The ID of the created row.
 */
::sqlite3_int64 createFaceClusterEntry(::sqlite3 *lightroomDB)
{
   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return -1;
   }

   TFSql sql(lightroomDB,
             "INSERT INTO AgLibraryFaceCluster "
             "       (id_local, keyFace) "
             "VALUES (? , NULL ) ");
   sql.bind(1, id_local);
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert cluster: " << sql.getErrorMsg() << std::endl;
      return -1;
   }

   return id_local;
}

/**
 * Creates the entries for one face.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param facedata      The data (position and person name) of the face to create.
 * @param clusterID     The ID of the AgLibraryFaceCluster entry.
 * @param imageID       The ID of the image that contains the face.
 * @param orientation   The orientation of the image (AB: portrait, BC:
 *                      clockwise, CD: upside-down, DA: counter-clockwise)
 * @return The ID of the created entry in the AgLibraryFace table.
 */
::sqlite3_int64 createFace(::sqlite3 *lightroomDB, facedata &facedata,
                           ::sqlite_int64 clusterID, ::sqlite_int64 imageID,
                           std::string orientation)
{
   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return -1;
   }

   double bl_x = facedata.bl_x;
   double bl_y = facedata.bl_y;
   double br_x = facedata.br_x;
   double br_y = facedata.br_y;
   double tl_x = facedata.tl_x;
   double tl_y = facedata.tl_y;
   double tr_x = facedata.tr_x;
   double tr_y = facedata.tr_y;

   // Aperture uses a slightly different coordinate system than Lightroom.
   // Convert here ...
   if (orientation == "AB") {
      bl_y = 1-facedata.bl_y;
      br_y = 1-facedata.br_y;
      tl_y = 1-facedata.tl_y;
      tr_y = 1-facedata.tr_y;
   } else if (orientation == "BC") {
      bl_x = facedata.bl_y;
      br_x = facedata.br_y;
      tl_x = facedata.tl_y;
      tr_x = facedata.tr_y;
      bl_y = facedata.bl_x;
      br_y = facedata.br_x;
      tl_y = facedata.tl_x;
      tr_y = facedata.tr_x;
   } else if (orientation == "CD") {
      bl_x = 1-facedata.bl_x;
      br_x = 1-facedata.br_x;
      tl_x = 1-facedata.tl_x;
      tr_x = 1-facedata.tr_x;
      bl_y = facedata.bl_y;
      br_y = facedata.br_y;
      tl_y = facedata.tl_y;
      tr_y = facedata.tr_y;
   } else if (orientation == "DA") {
      bl_x = 1-facedata.bl_y;
      br_x = 1-facedata.br_y;
      tl_x = 1-facedata.tl_y;
      tr_x = 1-facedata.tr_y;
      bl_y = 1-facedata.bl_x;
      br_y = 1-facedata.br_x;
      tl_y = 1-facedata.tl_x;
      tr_y = 1-facedata.tr_x;
   }

   TFSql sql(lightroomDB,
             "INSERT into AgLibraryFace "
             "            (id_local, "
             "             bl_x, bl_y, br_x, br_y, tl_x, tl_y, tr_x, tr_y, "
             "             cluster, compatibleVersion, ignored, image, imageOrientation, "
             "             orientation, origination, propertiesCache, regionType, "
             "             skipSuggestion, version) "
             "VALUES(?, "
             "       ?, ?, ?, ?, ?, ?, ?, ?, "
             "       ?, 3.0, NULL, ?, ?, "
             "       0, 1.0, NULL, 1.0, "
             "       NULL, 2.0)");
   sql.bind(1, id_local);
   sql.bind(2, bl_x);
   sql.bind(3, bl_y);
   sql.bind(4, br_x);
   sql.bind(5, br_y);
   sql.bind(6, tl_x);
   sql.bind(7, tl_y);
   sql.bind(8, tr_x);
   sql.bind(9, tr_y);
   sql.bind(10, clusterID);
   sql.bind(11, imageID);
   sql.bind(12, orientation);

   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert face data: " << sql.getErrorMsg() << std::endl;
      return -1;
   }

   return id_local;
}

/**
 * When Lightroom "learns" a new face, it stores the biometry data in the
 * AgLibraryFaceData table. We cannot transfer these data from Aperture but
 * having an empty entry is fine because that's what you get if you mark an
 * undetected face in Lightroom.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param faceID        The ID of the face in the AgLibraryFace table.
 * @return @c true on succes, @c false on any error.
 */
bool createFaceData(::sqlite3 *lightroomDB, ::sqlite3_int64 faceID)
{
   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return false;
   }

   TFSql sql(lightroomDB,
             "INSERT into AgLibraryFaceData "
             "            (id_local, data, face) "
             "VALUES(?, ?, ?)");
   sql.bind(1, id_local);
   sql.bind(2);
   sql.bind(3, faceID);

   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert face data: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

/**
 * Whenever you use a keyword in Lightroom, its popularity increases by the
 * current value of "LibraryKeywordSuggestions_popularityIncrement". This
 * increment then increments by 10%. That way, new keywords start quite high but
 * often used keywords decline slowly.
 *
 * See http://stackoverflow.com/questions/11128086/simple-popularity-algorithm
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param keywordID     The ID of the keyword.
 * @return @c true on succes, @c false on any error.
 */
bool incrementKeywordPopularity(::sqlite3 *lightroomDB, ::sqlite3_int64 keywordID)
{
   TFSql sql(lightroomDB,
             "SELECT value "
             "FROM Adobe_variablesTable "
             "WHERE name = 'LibraryKeywordSuggestions_popularityIncrement'");
   sql.step();
   double popularityStep = sql.column_double(0);

   if (sql.hasFailed()) {
      std::cerr << "Failed to read popularity base value: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   sql.reset("UPDATE Adobe_variablesTable "
             "SET value = ? "
             "WHERE name = 'LibraryKeywordSuggestions_popularityIncrement'");
   sql.bind(1, popularityStep * 1.1);
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to update popularity base value: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   ::sqlite3_int64 id_local;
   ::sqlite3_int64 occurrences;
   double popularity;

   sql.reset("SELECT id_local, occurrences, popularity "
             "FROM AgLibraryKeywordPopularity "
             "WHERE tag = ?");
   sql.bind(1, keywordID);
   if (!sql.step()) {
      if (sql.hasFailed()) {
         std::cerr << "Failed to find keyword in keyword popularity list: " << sql.getErrorMsg() << std::endl;
         return false;
      }

      // Keyword does not exist: Create
      id_local = getNextLocalID(lightroomDB);
      occurrences = 0;
      popularity = 0;
   } else {
      id_local = sql.column_int64(0);
      occurrences = sql.column_int64(1);
      popularity = sql.column_double(2);
   }

   occurrences++;
   popularity += popularityStep;

   sql.reset("INSERT OR REPLACE INTO AgLibraryKeywordPopularity "
             "(id_local, occurrences, popularity, tag) "
             "VALUES "
             "(?, ?, ?, ?)");
   sql.bind(1, id_local);
   sql.bind(2, occurrences);
   sql.bind(3, popularity);
   sql.bind(4, keywordID);
   sql.step();

   if (sql.hasFailed()) {
      std::cerr << "Failed to update/insert popularity in keyword popularity list: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

/**
 * This method adds a row in the AgLibraryKeywordImage table, assigning the
 * keyword to the given image.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param imageID       The ID of the image.
 * @param keywordID     The ID of the keyword.
 * @return @c true on succes, @c false on any error.
 */
bool createKeywordImage(::sqlite3 *lightroomDB, ::sqlite3_int64 imageID, ::sqlite3_int64 keywordID)
{
   TFSql sql(lightroomDB,
             "SELECT count(*) "
             "FROM AgLibraryKeywordImage "
             "WHERE image = ? "
             "AND tag = ? ");
   sql.bind(1, imageID);
   sql.bind(2, keywordID);

   sql.step();
   ::sqlite3_int64 count = sql.column_int64(0);
   if (sql.hasFailed()) {
      std::cerr << "Failed to select keyword image: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   if (count == 0) {
      ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
      if (id_local < 0) {
         return false;
      }

      TFSql sql(lightroomDB,
                "INSERT into AgLibraryKeywordImage "
                "            (id_local, image, tag) "
                "VALUES(?, ?, ?)");
      sql.bind(1, id_local);
      sql.bind(2, imageID);
      sql.bind(3, keywordID);

      sql.step();
      if (sql.hasFailed()) {
         std::cerr << "Failed to insert keyword image: " << sql.getErrorMsg() << std::endl;
         return false;
      }

      if (!incrementKeywordPopularity(lightroomDB, keywordID)) {
         return false;
      }
   }

   return true;
}

/**
 * Creates a row in the AgLibraryKeywordFace table, assigning the keyword to the
 * face data. We also mark the association as user generated.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param faceID        The ID of the face.
 * @param keywordID     The ID of the keyword.
 * @return @c true on succes, @c false on any error.
 */
bool createKeywordFace(::sqlite3 *lightroomDB, ::sqlite3_int64 faceID,
                       ::sqlite3_int64 keywordID)
{
   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return false;
   }

   TFSql sql(lightroomDB,
             "INSERT into AgLibraryKeywordFace "
             "            (id_local, face, keyFace, rankOrder, tag, userPick, userReject) "
             "VALUES(?, ?, NULL, NULL, ?, 1, 0)");
   sql.bind(1, id_local);
   sql.bind(2, faceID);
   sql.bind(3, keywordID);

   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert keyword face: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

/**
 * Lightroom tracks the list of images it has analysed for faces in the
 * Adobe_libraryImageFaceProcessHistory table. Right after the Aperture import
 * into an empty Lightroom catalog, this table is empty. Depending on your
 * setting, Lightroom might start to detect faces right away. In this case, the
 * table might contain entries for images that Aperture has face information
 * for. This method creates its own entry if necessary or highjacks the existing
 * one, marking the image as having face detection done and marking the image as
 * "user touched" (that way Lightroom will not re-run face detection if they
 * improve their algorithm).
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param image_id      The ID of the image to mark.
 * @param orientation   The image orientation.
 * @return @c true on succes, @c false on any error.
 */
bool createFaceProcessHistory(::sqlite3 *lightroomDB, ::sqlite_int64 image_id, std::string orientation)
{
   ::sqlite3_int64 id_local = -1;
   TFSql sql(lightroomDB,
             "SELECT id_local "
             "FROM Adobe_libraryImageFaceProcessHistory "
             "WHERE image = ?");
   sql.bind(1, image_id);
   if (sql.step()) {
      id_local = sql.column_int64(0);
   }
   if (sql.hasFailed()) {
      std::cerr << "Failed to find existing process history id: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   if (id_local == -1) {
      id_local = getNextLocalID(lightroomDB);
      if (id_local < 0) {
         return false;
      }

      sql.reset("INSERT INTO Adobe_libraryImageFaceProcessHistory "
                "            (id_local, image, "
                "             lastFaceDetector, lastFaceRecognizer, lastImageIndexer, "
                "             lastImageOrientation, lastTryStatus, userTouched) "
                "VALUES(?, ?, 2.0, 3.0, NULL, ?, 1.0, 1.0)");
      sql.bind(1, id_local);
      sql.bind(2, image_id);
      sql.bind(3, orientation);
   } else {
      sql.reset("UPDATE Adobe_libraryImageFaceProcessHistory "
                "SET userTouched = 1.0, "
                "    lastTryStatus = 1.0, "
                "    lastImageOrientation = ? "
                "WHERE id_local = ?");
      sql.bind(1, orientation);
      sql.bind(2, id_local);
   }

   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to insert process history: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

/**
 * If Lightroom got time to run face detection on the images imported from
 * Aperture, we might have faces found by Lightroom AND by Aperture. This tool
 * is designed to replace all faces by the ones defined in Aperture thus we
 * delete all information about faces found by Lightroom.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param image_id      The ID of the image whose face information to remove.
 * @return @c true on succes, @c false on any error.
 */
bool removeLightroomFacesForImage(::sqlite3 *lightroomDB, ::sqlite_int64 image_id)
{
   {
      TFSql sql(lightroomDB,
                "DELETE FROM AgLibraryKeywordImage "
                "WHERE image = ? "
                "AND tag IN (SELECT tag FROM AgLibraryKeywordFace WHERE face IN (SELECT id_local FROM AgLibraryFace WHERE image = ?))");
      sql.bind(1, image_id);
      sql.bind(2, image_id);
      sql.step();
      if (sql.hasFailed()) {
         std::cerr << "Failed to remove keywords: " << sql.getErrorMsg() << std::endl;
         return false;
      }
   }

   const char *removes[] = {
      "DELETE FROM Adobe_libraryImageFaceProcessHistory WHERE image = ?",
      "DELETE FROM AgLibraryFaceCluster WHERE id_local IN (SELECT cluster FROM AgLibraryFace WHERE image = ?)",
      "DELETE FROM AgLibraryFaceData WHERE face IN (SELECT id_local FROM AgLibraryFace WHERE image = ?)",
      "DELETE FROM AgLibraryKeywordFace WHERE face IN (SELECT id_local FROM AgLibraryFace WHERE image = ?)",
      "DELETE FROM AgLibraryFace WHERE image = ?"
   };

   for (int i = 0; i < (sizeof(removes)/sizeof(const char *)); ++i) {
      TFSql sql(lightroomDB,
                removes[i]);
      sql.bind(1, image_id);
      sql.step();
      if (sql.hasFailed()) {
         std::cerr << "Failed to execute " << removes[i] << ": " << sql.getErrorMsg() << std::endl;
         return false;
      }
   }

   return true;
}

/**
 * Main routine to create all data required for one new face entry.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param facedata      The data of the face to create.
 * @param image_id      The ID of the image the face belongs to.
 * @param orientation   The orientation of the image (in Lightroom style).
 * @return @c true on succes, @c false on any error.
 */
bool createFaceEntry(::sqlite3 *lightroomDB, facedata &facedata, ::sqlite_int64 image_id, std::string orientation)
{
   ::sqlite3_int64 keywordID = -1;
   if (facedata.name != "") {
      keywordID = findExistingKeywordID(lightroomDB, facedata.name);
      if (keywordID == -1) {
         keywordID = createNewKeyword(lightroomDB, facedata.name, getRootKeywordId(lightroomDB), "person");
      }
      if (keywordID == -1) {
         return false;
      }
   }
   ::sqlite3_int64 clusterID = createFaceClusterEntry(lightroomDB);
   if (clusterID == -1) {
      return false;
   }
   ::sqlite3_int64 faceID = createFace(lightroomDB, facedata, clusterID, image_id, orientation);
   if (faceID == -1) {
      return false;
   }
   if (!createFaceData(lightroomDB, faceID)) {
     return false;
   }
   if (keywordID != -1) {
       if (!createKeywordFace(lightroomDB, faceID, keywordID) ||
           !createKeywordImage(lightroomDB, image_id, keywordID)) {
          return false;
       }
   }
   if (!createFaceProcessHistory(lightroomDB, image_id, orientation)) {
      return false;
   }

   return true;
}

/**
 * Lightroom maintains a table of all keywords that are assigned together to one image.
 * This method updated the table for one pair of keywords.
 *
 * @param lightroomDB   The handle of the lightroom database.
 * @param tag1          The ID of one keyword.
 * @param tag2          The ID of another keyword.
 */
bool insertOrIncreaseCooccurence(::sqlite3 *lightroomDB,
                                 ::sqlite_int64 tag1,
                                 ::sqlite_int64 tag2)
{
   ::sqlite_int64 id_local = -1;
   ::sqlite_int64 count = 0;

   TFSql oldCount(lightroomDB,
                  "SELECT id_local, value "
                  "FROM AgLibraryKeywordCooccurrence "
                  "WHERE tag1 = ? "
                  "AND tag2 = ?");
   oldCount.bind(1, tag1);
   oldCount.bind(2, tag2);
   if (oldCount.step()) {
      id_local = oldCount.column_int64(0);
      count = oldCount.column_int64(1);
   }
   if (oldCount.hasFailed()) {
      std::cerr << "Failed to get old count of coocurrence: " << oldCount.getErrorMsg() << std::endl;
      return false;
   }

   if (id_local != -1) {
      TFSql update(lightroomDB,
                   "UPDATE AgLibraryKeywordCooccurrence "
                   "SET value = ? "
                   "WHERE id_local = ?");
      update.bind(1, ++count);
      update.bind(2, id_local);
      update.step();
      if (update.hasFailed()) {
         std::cerr << "Updateing Cooccurrence failed: " << update.getErrorMsg() << std::endl;
         return false;
      }
   } else {
      id_local = getNextLocalID(lightroomDB);
      if (id_local == -1) {
         return false;
      }
      TFSql insert(lightroomDB,
                   "INSERT INTO AgLibraryKeywordCooccurrence (id_local, tag1, tag2, value) "
                   " VALUES(?, ?, ?, 1)");
      insert.bind(1, id_local);
      insert.bind(2, tag1);
      insert.bind(3, tag2);
      insert.step();
      if (insert.hasFailed()) {
         std::cerr << "Inserting Cooccurrence failed: " << insert.getErrorMsg() << std::endl;
         return false;
      }
   }

   return true;
}

/**
 * Lightroom maintains a table of all keywords that are assigned together to one
 * image. We do not track each change but rebuild the whole table at the end.
 *
 * @param lightroomDB   The handle of the lightroom database.
 */
bool rebuildKeywordCoocurrences(::sqlite3 *lightroomDB)
{
   TFSql cleanup(lightroomDB,
                 "DELETE FROM AgLibraryKeywordCooccurrence");
   cleanup.step();
   if (cleanup.hasFailed()) {
      std::cerr << "Failed to remove old cooccurrences: " << cleanup.getErrorMsg() << std::endl;
      return false;
   }

   TFSql images(lightroomDB,
                "SELECT image "
                "FROM AgLibraryKeywordImage "
                "GROUP BY image "
                "HAVING COUNT(image) > 1 ");
   while (images.step()) {
      ::sqlite_int64 image_id = images.column_int64(0);

      TFSql keywordsOfImage(lightroomDB,
                            "SELECT tag "
                            "FROM AgLibraryKeywordImage "
                            "WHERE image = ?");
      keywordsOfImage.bind(1, image_id);

      std::vector<::sqlite_int64> tags;
      while(keywordsOfImage.step()) {
         tags.push_back(keywordsOfImage.column_int64(0));
      }

      for (int i = 0; i < tags.size()-1; ++i) {
         for (int j = i+1; j < tags.size(); ++j) {
            if (!insertOrIncreaseCooccurence(lightroomDB, tags[i], tags[j]) ||
                !insertOrIncreaseCooccurence(lightroomDB, tags[j], tags[i])) {
               return false;
            }
         }
      }
   }

   if (images.hasFailed()) {
      std::cerr << "Failed to set Coocurrences: " << images.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

// TODO document
bool removeAllKeywords(::sqlite3 *lightroomDB)
{
   TFSql sql1(lightroomDB, "DELETE FROM AgLibraryKeyword");
   TFSql sql2(lightroomDB, "DELETE FROM AgLibraryKeywordCooccurrence");
   TFSql sql3(lightroomDB, "DELETE FROM AgLibraryKeywordFace");
   TFSql sql4(lightroomDB, "DELETE FROM AgLibraryKeywordImage");
   TFSql sql5(lightroomDB, "DELETE FROM AgLibraryKeywordPopularity");
   TFSql sql6(lightroomDB, "DELETE FROM AgLibraryKeywordSynonym");

   sql1.step();
   sql2.step();
   sql3.step();
   sql4.step();
   sql5.step();
   sql6.step();

   if (sql1.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql1.getErrorMsg() << std::endl;
      return false;
   }
   if (sql2.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql2.getErrorMsg() << std::endl;
      return false;
   }
   if (sql3.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql3.getErrorMsg() << std::endl;
      return false;
   }
   if (sql4.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql4.getErrorMsg() << std::endl;
      return false;
   }
   if (sql5.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql5.getErrorMsg() << std::endl;
      return false;
   }
   if (sql6.hasFailed()) {
      std::cerr << "Failed to remove all keywords: " << sql6.getErrorMsg() << std::endl;
      return false;
   }
   return true;
}

::sqlite3_int64 findVersionIDForMaster(::sqlite3 *apertureDB,
                                       const std::string &masterUUID,
                                       const std::string &copyName)
{
   ::sqlite3_int64 copyNr = INT64_MAX;
   if (copyName.find("VERSION-") == 0) {
      copyNr = ::atoi(copyName.substr(8).c_str());
      if (copyNr > 0) {
         copyNr--;
      }
   }

   TFSql sql(apertureDB,
             "SELECT modelId "
             "FROM RKVersion "
             "WHERE masterUuid = ? "
             "AND versionNumber <= ? "
             "ORDER BY versionNumber DESC");
   sql.bind(1, masterUUID);
   sql.bind(2, copyNr);
   if (!sql.step()) {
      std::cerr << "Failed to find version ID from master UUID " << masterUUID << ", copy " << copyName << ":" << sql.getErrorMsg() << std::endl;
      return -1;
   }
   return sql.column_int64(0);
}

bool findKeywordsForVersion(std::deque<std::string> &result,
                            ::sqlite3 *apertureDB,
                            const std::string &fileName,
                            ::sqlite3_int64 imageDate,
                            const std::string &copyName)
{
   std::string masterUUID = findImageUUIDForFilename(apertureDB, fileName, imageDate);
   if (masterUUID != "") {
      ::sqlite3_int64 versionID = findVersionIDForMaster(apertureDB, masterUUID, copyName);
      if (versionID >= 0) {
         TFSql sql(apertureDB,
                   "SELECT K.name "
                   "FROM RKKeyword K, RKKeywordForVersion V "
                   "WHERE K.modelId = V.keywordId "
                   "AND V.versionId = ?");
         sql.bind(1, versionID);

         while (sql.step()) {
            result.push_back(sql.column_str(0));
         }

         if (!sql.hasFailed()) {
            return true;
         }
      }
   }

   return false;
}

bool recreateRootKeyword(::sqlite3 *lightroomDB,
                         std::string faceKeywordsRoot,
                         std::string tagKeywordsRoot)
{
   TFSql sql(lightroomDB,
            "SELECT value "
            "FROM Adobe_variablesTable "
            "WHERE name = 'AgLibraryKeyword_rootTagID'");
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to create keyword root: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   ::sqlite3_int64 id_local = sql.column_int64(0);
   if (id_local < 0) {
      std::cerr << "Failed to create keyword root: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   sql.reset("INSERT into AgLibraryKeyword(id_local, id_global, dateCreated, imageCountCache, keywordType, lastApplied, lc_name, name, parent) "
             "VALUES(?, ?, "
             "       (julianday('now') - 2440587.5)*86400.0 - strftime('%s','2001-01-01 00:00:00'), "
             "       NULL, "
             "       NULL, "
             "       NULL, "
             "       NULL, "
             "       NULL, "
             "       NULL)");
   sql.bind(1, id_local);
   sql.bind(2, uuid());
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to create root keyword: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   // Build genealogy string
   std::stringstream genealogy_stream;
   genealogy_stream << "/" << int(::log10(id_local)+1) << id_local;
   std::string genealogy = genealogy_stream.str();

   sql.reset("UPDATE AgLibraryKeyword "
             "SET genealogy = ? "
             "WHERE id_local = ?");
   sql.bind(1, genealogy);
   sql.bind(2, id_local);
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to set genealogy of keyword: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   ::sqlite3_int64 faceKeywordId = createNewKeyword(lightroomDB, faceKeywordsRoot, id_local, nullptr);
   if (0 > faceKeywordId) {
      std::cerr << "Failed to create face keywords root: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   sql.reset("SELECT value "
             "FROM Adobe_variablesTable "
             "WHERE name = 'AgLibraryKeywords_newPersonKeywordParent'");
   if (sql.step()) {
      sql.reset("UPDATE Adobe_variablesTable "
                "SET value = ? "
                "WHERE name = 'AgLibraryKeywords_newPersonKeywordParent'");
      sql.bind(1, faceKeywordId);
      sql.step();
   } else {
      sql.reset("INSERT INTO Adobe_variablesTable (id_local, id_global, name, type, value) "
                "VALUES (?, ?, 'AgLibraryKeywords_newPersonKeywordParent', NULL, ?)");
      sql.bind(1, getNextLocalID(lightroomDB));
      sql.bind(2, uuid());
      sql.bind(3, faceKeywordId);
      sql.step();
   }
   if (sql.hasFailed()) {
      std::cerr << "Failed to set face keyword group as default for new faces" << std::endl;
   }

   if (tagKeywordsRoot != "") {
      ::sqlite3_int64 tagsKeywordId = createNewKeyword(lightroomDB, tagKeywordsRoot, id_local, nullptr);
      if (tagsKeywordId < 0) {
         std::cerr << "Failed to create tag keywords root: " << sql.getErrorMsg() << std::endl;
         return false;
      }

      sql.reset("SELECT value "
                "FROM Adobe_variablesTable "
                "WHERE name = 'AgLibraryKeywords_newKeywordParent'");
      if (sql.step()) {
         sql.reset("UPDATE Adobe_variablesTable "
                   "SET value = ? "
                   "WHERE name = 'AgLibraryKeywords_newKeywordParent'");
         sql.bind(1, tagsKeywordId);
         sql.step();
      } else {
         sql.reset("INSERT INTO Adobe_variablesTable (id_local, id_global, name, type, value) "
                   "VALUES (?, ?, 'AgLibraryKeywords_newKeywordParent', NULL, ?)");
         sql.bind(1, getNextLocalID(lightroomDB));
         sql.bind(2, uuid());
         sql.bind(3, tagsKeywordId);
         sql.step();
      }
      if (sql.hasFailed()) {
         std::cerr << "Failed to set tag keyword group as default for new tags" << std::endl;
      }
   }

   return true;
}

bool connectKeywordWithImage(::sqlite3 *lightroomDB,
                             ::sqlite3_int64 versionID,
                             ::sqlite3_int64 keywordID)
{
   ::sqlite3_int64 id_local = getNextLocalID(lightroomDB);
   if (id_local < 0) {
      return false;
   }

   TFSql sql(lightroomDB,
             "INSERT INTO AgLibraryKeywordImage(id_local, image, tag) "
             "VALUES(?, ?, ?)");
   sql.bind(1, id_local);
   sql.bind(2, versionID);
   sql.bind(3, keywordID);

   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to connect keyword with image: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   return incrementKeywordPopularity(lightroomDB, keywordID);
}

bool recreateKeywords(::sqlite3 *lightroomDB,
                      std::map<::sqlite3_int64, std::deque<std::string>> keywordsMap)
{
   std::map<std::string, ::sqlite3_int64> knownKeywords;

   for (std::pair<::sqlite3_int64, std::deque<std::string>> i : keywordsMap) {
      ::sqlite3_int64 imageID = i.first;
      std::deque<std::string> keywords = i.second;

      for (std::string keyword : keywords) {
         keyword = normalizeUTF8(keyword);
         // std::cout << "Recreating keyword " << keyword << std::endl;

         ::sqlite3_int64 keywordID = -1;

         auto iter = knownKeywords.find(keyword);
         if (iter != knownKeywords.end()) {
            keywordID = iter->second;
         } else {
            keywordID = createNewKeyword(lightroomDB,
                                         keyword,
                                         getTagRootKeywordId(lightroomDB),
                                         nullptr);
            std::cout << "Created keyword `" << keyword << "'" << std::endl;

            knownKeywords.insert(std::pair<std::string, ::sqlite3_int64>(keyword, keywordID));
         }

         if (keywordID == -1) {
            std::cerr << "Failed to create keyword: " << keyword << std::endl;
            return false;
         }

         if (!connectKeywordWithImage(lightroomDB, imageID, keywordID)) {
            std::cerr << "Failed to connect image with keyword" << std::endl;
         }
      }
   }

   return true;
}

bool removeAllStacks(::sqlite3 *lightroomDB)
{
   TFSql sql1(lightroomDB, "DELETE FROM AgLibraryFolderStack");
   TFSql sql2(lightroomDB, "DELETE FROM AgLibraryFolderStackData");
   TFSql sql3(lightroomDB, "DELETE FROM AgLibraryFolderStackImage");

   sql1.step();
   sql2.step();
   sql3.step();

   if (sql1.hasFailed()) {
      std::cerr << "Failed to remove all stacks: " << sql1.getErrorMsg() << std::endl;
      return false;
   }
   if (sql2.hasFailed()) {
      std::cerr << "Failed to remove all stacks: " << sql2.getErrorMsg() << std::endl;
      return false;
   }
   if (sql3.hasFailed()) {
      std::cerr << "Failed to remove all stacks: " << sql3.getErrorMsg() << std::endl;
      return false;
   }

   return true;
}

std::string findApertureStackIdOfVersion(::sqlite3 *apertureDB,
                                         const std::string &fileName,
                                         ::sqlite3_int64 imageDate,
                                         const std::string &copyName)
{
   std::string stackUuid;

   std::string masterUUID = findImageUUIDForFilename(apertureDB, fileName, imageDate);
   if (masterUUID != "") {
      ::sqlite3_int64 copyNr = INT64_MAX;
      if (copyName.find("VERSION-") == 0) {
         copyNr = ::atoi(copyName.substr(8).c_str());
         if (copyNr > 0) {
            copyNr--;
         }
      }

      TFSql sql(apertureDB,
                "SELECT stackUuid "
                "FROM RKVersion "
                "WHERE masterUuid = ? "
                "AND versionNumber <= ? "
                "ORDER BY versionNumber DESC");
      sql.bind(1, masterUUID);
      sql.bind(2, copyNr);
      if (sql.step()) {
         stackUuid = sql.column_str(0);
      } else {
         std::cerr << "Didn't find stack UUID for " << fileName << std::endl;
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to get stack UUID" << std::endl;
         return "";
      }
   } else {
      std::cerr << "Didn't find master UUID for " << fileName << std::endl;
   }

   return stackUuid;
}

// TODO: Doc
bool createStack(::sqlite3 *lightroomDB,
                 std::deque<::sqlite3_int64> images)
{
   ::sqlite3_int64 id_local_stack = getNextLocalID(lightroomDB);
   if (-1 == id_local_stack) {
      return false;
   }
   TFSql sql(lightroomDB,
             "INSERT INTO AgLibraryFolderStack(id_local, id_global, collapsed, text) "
             "VALUES (?, ?, 1, '')");
   sql.bind(1, id_local_stack);
   sql.bind(2, uuid());
   sql.step();
   if (sql.hasFailed()) {
      std::cerr << "Failed to create empty stack: " << sql.getErrorMsg() << std::endl;
      return false;
   }

   for (int n = 0; n < images.size(); ++n) {
      ::sqlite3_int64 id_local_image = getNextLocalID(lightroomDB);
      if (id_local_image == -1) {
         return false;
      }
      sql.reset("INSERT INTO AgLibraryFolderStackImage(id_local, collapsed, image, position, stack) "
                "VALUES(?, 1, ?, ?, ?)");
      sql.bind(1, id_local_image);
      sql.bind(2, images[n]);
      sql.bind(3, (::sqlite3_int64) n+1);
      sql.bind(4, id_local_stack);
      sql.step();
      if (sql.hasFailed()) {
         std::cerr << "Failed to attach image to stack: " << sql.getErrorMsg() << std::endl;
         return false;
      }
   }

   return true;
}

// TODO: Doc
bool createStacks(::sqlite3 *lightroomDB,
                  std::map<std::string, std::deque<::sqlite3_int64>> stacks)
{
   for (auto stack : stacks) {
      if (!createStack(lightroomDB, stack.second)) {
         return false;
      }

      std::cout << "Created stack of " << stacks.size() << " images." << std::endl;
   }

   return true;
}

static xmlNsPtr xmlReconciliedNS(xmlDocPtr doc, xmlNodePtr tree, const char *href, const char *prefix) {
   xmlNsPtr def = xmlSearchNsByHref(doc, tree, (const xmlChar *) href);
   if (def) {
      return def;
   }

   xmlChar prefixBuffer[256];
   ::snprintf((char *)prefixBuffer, sizeof(prefixBuffer), "%.200s", prefix);
   def = xmlSearchNs(doc, tree, prefixBuffer);
   for (int counter = 0; def && counter < INT16_MAX; ++counter) {
      ::snprintf((char *)prefixBuffer, sizeof(prefixBuffer), "%.200s%d", prefix, counter);
      def = xmlSearchNs(doc, tree, prefixBuffer);
   }

   return xmlNewNs(tree, (const xmlChar *) href, prefixBuffer);
}

static xmlNodePtr xmlFindNode(xmlNodePtr node, const char *tag) {
   if (node) {
      xmlNodePtr current = node->children;
      while (current) {
         if (0 == ::strcmp((const char *)current->name, tag)) {
            return current;
         }

         current = current->next;
      }
   }

   return NULL;
}

bool xmlRemoveProp(xmlNodePtr node,
                   xmlNsPtr ns,
                   const char *name)
{
   xmlAttrPtr property = node->properties;
   while (property) {
      if (0 == ::strcmp(name, (char *)property->name) &&
          property->ns &&
          0 == ::strcmp((char *)ns->href, (char *)property->ns->href))  {

         return 0 == xmlRemoveProp(property);
      }

      property = property->next;
   }

   return true;
}

bool updateXmp(std::string &xmp,
               double latitude,
               double longitude)
{
   bool result = false;

   static bool initialized = false;
   if (!initialized) {
      initialized = true;

      LIBXML_TEST_VERSION
   }

   xmlDocPtr doc = xmlReadMemory(xmp.c_str(), xmp.size(), "/", NULL, XML_PARSE_NONET);
   if (doc) {
      xmlNode *root_element = xmlDocGetRootElement(doc);
      if (root_element) {
         xmlNodePtr rdfNode = xmlFindNode(root_element, "RDF");
         if (rdfNode) {
            xmlNodePtr descriptionNode = xmlFindNode(rdfNode, "Description");

            xmlNsPtr exifNS = xmlReconciliedNS(doc, descriptionNode, "http://ns.adobe.com/exif/1.0/", "exif");
            if (exifNS) {
               char latitudeStr[256];
               char longitudeStr[256];

               const char *northSouth = latitude >= 0 ? "N" : "S";
               const char *eastWest = longitude >= 0 ? "E" : "W";

               latitude = latitude < 0 ? -latitude : latitude;
               longitude = longitude < 0 ? -longitude : longitude;

               ::snprintf(latitudeStr, sizeof latitudeStr, "%d,%.10lf%s", (int) latitude, (latitude - (int)latitude)*60, northSouth);
               ::snprintf(longitudeStr, sizeof longitudeStr, "%d,%.10lf%s", (int) longitude, (longitude - (int) longitude)*60, eastWest);

               if (xmlSetNsProp(descriptionNode, exifNS, (xmlChar*) "GPSVersionID", (xmlChar *)"2.0.0.0") &&
                   xmlSetNsProp(descriptionNode, exifNS, (xmlChar*) "GPSLatitude", (xmlChar *)latitudeStr) &&
                   xmlSetNsProp(descriptionNode, exifNS, (xmlChar*) "GPSLongitude", (xmlChar *)longitudeStr) &&
                   xmlSetNsProp(descriptionNode, exifNS, (xmlChar*) "GPSLatitudeRef", (xmlChar *)northSouth) &&
                   xmlSetNsProp(descriptionNode, exifNS, (xmlChar*) "GPSLongitudeRef", (xmlChar *)eastWest) &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSAltitude") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSAltitudeRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSAreaInformation") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDOP") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDateStamp") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestBearing") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestBearingRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestDistance") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestDistanceRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestLatitude") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestLatitudeRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestLongitude") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDestLongitudeRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSDifferential") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSHPositioningError") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSImgDirection") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSImgDirectionRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSMapDatum") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSMeasureMode") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSProcessingMethod") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSSatellites") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSSpeed") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSSpeedRef") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSStatus") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSTimeStamp") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSTrack") &&
                   xmlRemoveProp(descriptionNode, exifNS, "GPSTrackRef")) {

                  xmlChar *xml = NULL;
                  int xmlSize = 0;

                  xmlDocDumpFormatMemoryEnc(doc,
                                            &xml,
                                            &xmlSize,
                                            "UTF-8",
                                            1);

                  if (xml) {
                     result = true;
                     xmp = std::string((const char *) xml, xmlSize);

                     xmlFree(xml);
                  }
               }
            }
         }
      }

      xmlFreeDoc(doc);
   }

   return result;
}

bool transferGPS(::sqlite3 *apertureDB,
                 ::sqlite3 *lightroomDB,
                 ::sqlite3_int64 image_id,
                 const std::string &fileName,
                 ::sqlite3_int64 imageDate,
                 const std::string &copyName)
{
   std::string masterUUID = findImageUUIDForFilename(apertureDB, fileName, imageDate);
   if (masterUUID != "") {
      ::sqlite3_int64 copyNr = INT64_MAX;
      if (copyName.find("VERSION-") == 0) {
         copyNr = ::atoi(copyName.substr(8).c_str());
         if (copyNr > 0) {
            copyNr--;
         }
      }

      TFSql sql(apertureDB,
                "SELECT exifLatitude, exifLongitude "
                "FROM RKVersion "
                "WHERE masterUuid = ? "
                "AND versionNumber <= ? "
                "ORDER BY versionNumber DESC");
      sql.bind(1, masterUUID);
      sql.bind(2, copyNr);
      if (sql.step() && !sql.column_null(0) && !sql.column_null(1)) {
         double latitude = sql.column_double(0);
         double longitude = sql.column_double(1);

         TFSql update(lightroomDB,
                      "UPDATE AgHarvestedExifMetadata "
                      "SET gpsLatitude = ?, "
                      "    gpsLongitude = ?, "
                      "    gpsSequence = 1, "
                      "    hasGPS = 1 "
                      "WHERE image = ?");
         update.bind(1, latitude);
         update.bind(2, longitude);
         update.bind(3, image_id);
         update.step();
         if (update.hasFailed()) {
            std::cerr << "Failed to update GPS information " << update.getErrorMsg() << std::endl;
            return false;
         }

         // Update Adobe_AdditionalMetadata
         TFSql findXMP(lightroomDB,
                       "SELECT xmp "
                       "FROM Adobe_AdditionalMetadata "
                       "WHERE image = ?");
         findXMP.bind(1, image_id);
         if (findXMP.step()) {
            std::string xmp = findXMP.column_str(0);

            if (findXMP.hasFailed()) {
               std::cerr << "Failed to read XMP data" << std::endl;
               return false;
            }

            // std::cerr << "--- Before ---------------------------------------" << std::endl;
            // std::cerr << xmp << std::endl;
            // std::cerr << "--------------------------------------------------" << std::endl;

            if (!updateXmp(xmp, latitude, longitude)) {
               std::cerr << "Failed to update XMP data" << std::endl;
               return false;
            }

            // std::cerr << "--- After ----------------------------------------" << std::endl;
            // std::cerr << xmp << std::endl;
            // std::cerr << "--------------------------------------------------" << std::endl;

            // Save updated XMP
            TFSql updateXMP(lightroomDB,
                            "UPDATE Adobe_AdditionalMetadata "
                            "SET xmp = ? "
                            "WHERE image = ?");
            updateXMP.bind(1, xmp);
            updateXMP.bind(2, image_id);
            updateXMP.step();
            if (updateXMP.hasFailed()) {
               std::cerr << "Failed to update XMP data" << std::endl;
               return false;
            }
         } else {
            std::cerr << "Warning: Did not find additional metadata" << std::endl;
         }
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to get GPS location" << std::endl;
         return false;
      }
   } else {
      std::cerr << "Didn't find master UUID for " << fileName << std::endl;
   }

   return true;
}

/**
 * Main.
 *
 * Parses command line arguments, iterates over all images in the Lightroom
 * database, searches for the image in the Aperture database, extracts all face
* information for the images and transfers them into the Lightroom database.
 */
int main(int argc, char *argv[])
{
   std::string lightroomDBFile = "./Lightroom Catalog.lrcat";
   std::string apertureDBFile =
      std::string(::getenv("HOME")) + "/Pictures/Aperture Library.aplibrary/Database/Library.apdb";
   std::string facesDBFile =
      std::string(::getenv("HOME")) + "/Pictures/Aperture Library.aplibrary/Database/Faces.db";

   int optchar;
   while (-1 != (optchar = getopt(argc, argv, "l:a:k:"))) {
      switch(optchar) {
         case 'l':
            lightroomDBFile = optarg;
            break;
         case 'a':
            apertureDBFile = optarg;
            apertureDBFile += "/Database/Library.apdb";
            facesDBFile = optarg;
            facesDBFile += "/Database/Faces.db";
            break;
         case 'f':
            g_keywordsRoot = optarg;
            break;
         case 't':
            g_tagKeywordsRoot = optarg;
            break;
         case 'h':
         default:
            std::cerr << "Usage: " << std::endl;
            std::cerr << "   " << argv[0] << " -l <Lightroom Catalog.lrcat> -a <Aperture Library.aplibrary> -k <Parent Of Keywords>" << std::endl;
            std::cerr << std::endl;
            std::cerr << "-l <file>   The Lightroom Catalog main file" << std::endl;
            std::cerr << "            (default: Lightroom Catalog.lrcat)" << std::endl;
            std::cerr << "-a <file>   The Aperture library bundle" << std::endl;
            std::cerr << "            (default: $HOME/Pictures/Aperture Library.aplibrary)" << std::endl;
            std::cerr << "-f <folder> The keywords folder to place face tags into" << std::endl;
            std::cerr << "            (default: Faces from Aperture)" << std::endl;
            std::cerr << "-t <folder> The keywords folder to place other keywords tags into" << std::endl;
            std::cerr << "            (default: Tags from Aperture)" << std::endl;
            ::exit(1);
      }
   }

   std::cout << std::endl << "### Opening database" << std::endl << std::endl;

   std::cout << "              Lightroom Catalog: " << lightroomDBFile << std::endl;
   std::cout << "      Aperture Library database: " << apertureDBFile << std::endl;
   std::cout << "        Aperture Faces database: " << facesDBFile << std::endl;
   std::cout << "Parent folder for face keywords: " << g_keywordsRoot << std::endl;
   std::cout << " Parent folder for tag keywords: " << g_tagKeywordsRoot << std::endl;

   ::sqlite3 *lightroomDB = NULL;
   ::sqlite3 *apertureDB = NULL;
   ::sqlite3 *facesDB = NULL;

   if (SQLITE_OK != ::sqlite3_open_v2(lightroomDBFile.c_str(), &lightroomDB, SQLITE_OPEN_READWRITE, NULL)) {
      std::cerr << "Can't open lightroom database: " << ::sqlite3_errmsg(lightroomDB) << std::endl;
      goto fail;
   }
   sqlite3_exec(lightroomDB, "BEGIN", 0, 0, 0);

   if (SQLITE_OK != ::sqlite3_open_v2(apertureDBFile.c_str(), &apertureDB, SQLITE_OPEN_READONLY, NULL)) {
      std::cerr << "Can't open aperture main database: " << ::sqlite3_errmsg(apertureDB) << std::endl;
      goto fail;
   }

   if (SQLITE_OK != ::sqlite3_open_v2(facesDBFile.c_str(), &facesDB, SQLITE_OPEN_READONLY, NULL)) {
      std::cerr << "Can't open aperture faces database: " << ::sqlite3_errmsg(facesDB) << std::endl;
      goto fail;
   }

   std::cout << std::endl << "### Preparing database" << std::endl << std::endl;

   std::cout << "Removing keywords" << std::endl;
   if (!removeAllKeywords(lightroomDB)) {
      std::cerr << "Failed to remove all keywords from lightroom" << std::endl;
      goto fail;
   }

   std::cout << "Recreating keyword roots" << std::endl;
   if (!recreateRootKeyword(lightroomDB, g_keywordsRoot, g_tagKeywordsRoot)) {
      std::cerr << "Failed to create keywords roots" << std::endl;
      goto fail;
   }

   std::cout << "Removing stacks" << std::endl;
   if (!removeAllStacks(lightroomDB)) {
      std::cerr << "Failed to remove all keywords from lightroom" << std::endl;
      goto fail;
   }

   {
      std::map<std::string, std::deque<::sqlite_int64>> stacksByApertureStackID;
      std::map<::sqlite_int64, std::deque<std::string>> keywordsByImage;
      std::map<std::string, int> insertedPeople;
      ::sqlite_int64 insertedFaces = 0;
      ::sqlite_int64 imagesCount = 0;
      ::sqlite_int64 imagesWithoutFaces = 0;
      ::sqlite_int64 unknownFaces = 0;

      std::cout << std::endl << "### Transfering face information" << std::endl << std::endl;
      TFSql sql(lightroomDB,
                "SELECT F.originalFilename, I.id_local, I.orientation, F.externalModTime, I.copyName "
                "FROM Adobe_images I, AgLibraryFile F, AgLibraryFolder O, AgLibraryRootFolder R "
                "WHERE F.id_local = I.rootFile "
                "AND O.id_local = F.folder "
                "AND R.id_local = O.rootFolder");

      while(sql.step()) {
         std::string fileName = sql.column_str(0);
         ::sqlite3_int64 image_id = sql.column_int64(1);
         std::string orientation = sql.column_str(2);
         ::sqlite3_int64 imageDate = sql.column_int64(3);
         std::string copyName = sql.column_str(4);

         imagesCount++;

         std::deque<facedata> faces = findFacesForImage(apertureDB, facesDB, fileName, imageDate);
         if (faces.size()) {
            std::cout << fileName << ": ";
            if (!removeLightroomFacesForImage(lightroomDB, image_id)) {
               return false;
            }
            std::string sep = "";
            for(facedata &face : faces) {
               if (!createFaceEntry(lightroomDB, face, image_id, orientation)) {
                  std::cerr << "Failed to create face entry" << std::endl;
               } else {
                  insertedFaces++;

                  if (face.name != "") {
                     auto iter = insertedPeople.find(face.name);
                     if (iter == insertedPeople.end()) {
                        insertedPeople.insert(std::pair<std::string,int>(face.name, 1));
                     } else {
                        iter->second++;
                     }
                  } else {
                     unknownFaces++;
                  }
               }

               std::cout << sep;
               if (face.name == "") {
                  std::cout << "[Unnamed]";
                  unknownFaces++;
               } else {
                  std::cout << face.name;
               }
               sep = ", ";
            }
            std::cout << std::endl;
         } else {
            imagesWithoutFaces++;
         }

         std::deque<std::string> keywordsForVersion;
         if (!findKeywordsForVersion(keywordsForVersion, apertureDB, fileName, imageDate, copyName)) {
            std::cerr << "Failed to get keywords for version" << std::endl;
         }
         keywordsByImage.insert(std::pair<::sqlite3_int64, std::deque<std::string>>(image_id, keywordsForVersion));

         std::string apertureStackId = findApertureStackIdOfVersion(apertureDB, fileName, imageDate, copyName);
         if (apertureStackId != "") {
            stacksByApertureStackID[apertureStackId].push_back(image_id);
         }

         if (!transferGPS(apertureDB, lightroomDB, image_id, fileName, imageDate, copyName)) {
            std::cerr << "Failed to transfer GPS location for version " << fileName << ", " << copyName << std::endl;
         }
      }

      if (sql.hasFailed()) {
         std::cerr << "Failed to read image: " << sql.getErrorMsg() << std::endl;
         goto fail;
      }

      std::cout << std::endl << "### Creating Stacks" << std::endl << std::endl;

      if (!createStacks(lightroomDB, stacksByApertureStackID)) {
         std::cerr << "Failed to create image stacks" << std::endl;
         goto fail;
      }

      std::cout << std::endl << "### Recreating keywords" << std::endl << std::endl;

      if (!recreateKeywords(lightroomDB, keywordsByImage)) {
         std::cerr << "Failed to recreate keywords." << std::endl;
         goto fail;
      }

      if (!fixKeywordsUTF8(lightroomDB)) {
         std::cerr << "Failed to fix keyword UTF-8 encoding to be composed" << std::endl;
         goto fail;
      }

      std::cout << std::endl << "### Cleaning up keyword coocurrences" << std::endl << std::endl;

      if (!rebuildKeywordCoocurrences(lightroomDB)) {
         std::cerr << "Failed to fix keyword coocurrences" << std::endl;
         goto fail;
      }

      std::cout << std::endl << "### Statistics" << std::endl << std::endl;
      std::cout << "Analysed " << imagesCount << " images, " << imagesWithoutFaces << " did not have any face information." << std::endl;
      std::cout << "Inserted " << insertedFaces << " faces from " << insertedPeople.size() << " people: ";
      std::cout << "[Unknown faces] (" << unknownFaces << ")";
      for (std::pair<std::string, int> p : insertedPeople) {
         std::cout << ", " << p.first << " (" << p.second << ")";
      }
      std::cout << std::endl;
   }

   sqlite3_exec(lightroomDB, "COMMIT", 0, 0, 0);

   std::cout << std::endl << "### Done" << std::endl << std::endl;
   std::cout << "Looks good." << std::endl;
fail:
   ::sqlite3_close(lightroomDB);
   ::sqlite3_close(apertureDB);
   ::sqlite3_close(facesDB);
}
