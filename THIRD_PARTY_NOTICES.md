# Third-party notices

Pettime is derived from [haichaojiang01-stack/pettime](https://github.com/haichaojiang01-stack/pettime).
The original copyright and MIT license are retained in `LICENSE`. The existing PNG assets are retained from that repository.

The C++ application uses Qt 6 Core, Gui, Widgets and Network, and Qt Test for development.
Qt is a separate dependency, not covered by Pettime's MIT license. Use these modules under
their applicable LGPL v3 / GPL terms or a commercial Qt license. This project defaults to
shared Qt libraries. Linux builds use system libraries; the Windows packaging script copies
Qt, its plugins and their runtime dependencies from the selected MSYS2 UCRT64 installation.
The ZIP includes the installed license texts in `licenses/` and a SHA-256 file manifest.
MSYS2 package metadata and source build recipes are available at https://packages.msys2.org/.
Qt source archives are available at https://download.qt.io/official_releases/qt/.

When redistributing Qt binaries, include the applicable Qt and third-party notices and
license texts, provide corresponding Qt source or a compliant source offer, and preserve
the user's ability to replace/relink the LGPL libraries. Dynamic linking alone does not
satisfy all LGPL obligations. Check the exact Qt build and plugins included in each package.

- [Qt licensing](https://doc.qt.io/qt-6/licensing.html)
- [Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
- [Third-party licenses used by Qt](https://doc.qt.io/qt-6/licenses-used-in-qt.html)
