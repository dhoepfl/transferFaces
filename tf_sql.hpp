#ifndef __TF_SQL__
#define __TF_SQL__

#include <sqlite3.h>

/**
 * A very simple wrapper around SQLite
 */
class TFSql
{
protected:
   ::sqlite3 *db;                ///< The SQLite database handle.
   ::sqlite3_stmt *statement;    ///< The statement we are working with.
   bool failed;                  ///< Flag is an error has occurred.
   std::string errorMsg;         ///< The error message if an error has occurred.

public:
   /**
    * Constructor.
    *
    * @param database   The database handle.
    * @param sql        The SQL template to build a statement for.
    */
   TFSql(::sqlite3 *database,
         const std::string &sql)
   : db(database), statement(NULL), failed(false), errorMsg()
   {
      if (SQLITE_OK != ::sqlite3_prepare_v2(db,
                                            sql.c_str(),
                                            -1,
                                            &statement,
                                            NULL)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }
   }

   /**
    * Destructor.
    *
    * Closes the statement, if still open.
    */
   ~TFSql()
   {
      if (statement) {
         ::sqlite3_finalize(statement);
      }
   }

   /**
    * Resets the statement to a new SQL template.
    *
    * All errors are cleared.
    *
    * @param sql  The SQL template to build a statement for.
    */
   void reset(const std::string &sql)
   {
      if (statement) {
         ::sqlite3_finalize(statement);
         statement = NULL;
      }
      failed = false;
      errorMsg = "";

      if (SQLITE_OK != ::sqlite3_prepare_v2(db,
                                            sql.c_str(),
                                            -1,
                                            &statement,
                                            NULL)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }
   }

   /**
    * Binds the given template parameter to NULL.
    *
    * @param index The index of the template parameter (1-based).
    * @return @c true on success, @c false when in error state.
    */
   bool bind(int index) {
      if (failed) return false;
      if (!statement) return false;

      if (SQLITE_OK != ::sqlite3_bind_null(statement, index)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }

      return !failed;
   }

   /**
    * Binds the given template parameter to the given value.
    *
    * @param index The index of the template parameter (1-based).
    * @param i     The value to set the parameter to.
    * @return @c true on success, @c false when in error state.
    */
   bool bind(int index, ::sqlite3_int64 i)
   {
      if (failed) return false;
      if (!statement) return false;

      if (SQLITE_OK != ::sqlite3_bind_int64(statement, index, i)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }

      return !failed;
   }

   /**
    * Binds the given template parameter to the given value.
    *
    * @param index The index of the template parameter (1-based).
    * @param str   The value to set the parameter to.
    * @return @c true on success, @c false when in error state.
    */
   bool bind(int index, std::string str)
   {
      if (failed) return false;
      if (!statement) return false;

      if (SQLITE_OK != ::sqlite3_bind_text(statement, index, (const char *) str.c_str(), -1, SQLITE_TRANSIENT)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }

      return !failed;
   }

   /**
    * Binds the given template parameter to the given value.
    *
    * @param index The index of the template parameter (1-based).
    * @param d     The value to set the parameter to.
    * @return @c true on success, @c false when in error state.
    */
   bool bind(int index, double d)
   {
      if (failed) return false;
      if (!statement) return false;

      if (SQLITE_OK != ::sqlite3_bind_double(statement, index, d)) {
         failed = true;
         errorMsg = ::sqlite3_errmsg(db);
      }

      return !failed;
   }

   /**
    * Execute one step of SQL processing (one row).
    *
    * @return @c false if error or no more data, @c true if data was read.
    */
   bool step(void)
   {
      if (failed) return false;
      if (!statement) return false;

      int ret = ::sqlite3_step(statement);
      if (SQLITE_ROW == ret) {
         return true;
      }
      if (SQLITE_DONE == ret) {
         return false;
      }

      failed = true;
      errorMsg = ::sqlite3_errmsg(db);

      return false;
   }

   /**
    * Read a column from the current row.
    *
    * @param index   The index of the column (0-based).
    * @return The value of the column or -1 when in error state.
    */
   ::sqlite3_int64 column_int64(int index)
   {
      if (failed) return -1;
      if (!statement) return -1;

      return ::sqlite3_column_int64(statement, index);
   }

   /**
    * Read a column from the current row.
    *
    * @param index   The index of the column (0-based).
    * @return The value of the column or empty string when in error state.
    */
   std::string column_str(int index)
   {
      if (failed) return "";
      if (!statement) return "";

      const char *txt = (const char *)::sqlite3_column_text(statement, index);
      if (!txt) {
         return "";
      }
      return txt;
   }

   /**
    * Read a column from the current row.
    *
    * @param index   The index of the column (0-based).
    * @return The value of the column or 0.0 when in error state.
    */
   double column_double(int index)
   {
      if (failed) return 0.0;
      if (!statement) return 0.0;

      return ::sqlite3_column_double(statement, index);
   }

   /**
    * Read a column from the current row.
    *
    * @param index   The index of the column (0-based).
    * @return The value of the column or true when in error state.
    */
   bool column_null(int index)
   {
      if (failed) return true;
      if (!statement) return true;

      return ::sqlite3_column_type(statement, index) == SQLITE_NULL;
   }

   /**
    * Checks whether the statement is in error state.
    *
    * @return @c true when in error state, @c false else.
    */
   bool hasFailed(void) { return failed; }

   /**
    * The error message when error state was entered.
    *
    * @return The error message.
    */
   std::string getErrorMsg(void) { return errorMsg; }
};

#endif

