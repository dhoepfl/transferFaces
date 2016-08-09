# What's transferFaces?

TransferFaces is a quick and dirty hack to transfer the faces marked in Aperture into a Lightroom catalog right after importing the pictures from Aperture. Read the warning below!

# Warning

This tool *deletes* data from your Lightroom catalog. I repeat: It deletes data.

The following data are deleted (on purpose, others might be deleted by accident):

 * All keywords. (Lightrooms Aperture import is crappy as hell, keywords are broken to being useless)
 * All stacks. (See above.)
 * All recognized faces for images that were detected to have face recognition run in Aperture.

# How to use it.

(You need Apple’s Developer Tools installed.)

1. Create with a new Lightroom catalog. (File → New Catalog…).
2. Import your Aperture library (File → Plug-in Extras → Import from Aperture Library)
2. Exit Lightroom.
3. Open Terminal.app.
4. Change to the transferFaces source directory: “cd <Drop source directory into Terminal window>”
5. Compile transferFaces: “clang++ -std=c++11 -o transferFaces transferFaces.cpp -lsqlite3 -framework CoreFoundation -lstdc++”
6. Run transferFaces: “./transferFaces -l <Drop the Lightroom catalog main file here (the one that ends in .lrcat)> -a <Drop your Aperture bundle (ends in .aplibrary) here>”
7. If the last line it prints is “Looks good.”, things look good.
8. Open Lightroom.
9. Go to the faces view and start face recognition, full library or on demand, does not matter, all images imported from Aperture have been marked as processed by face recognition.

# License

All rights reserved.

With the exception of Adobe or any company that is involved in creating Lightroom, the following license applies:

The MIT License (MIT)
Copyright (c) 2016 Daniel Höpfl <daniel@hoepfl.de>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
