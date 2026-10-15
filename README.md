# MyFiles

A phone-first index of your paper documents. Every envelope or box file gets a **file number**, either handwritten or on a printed QR label. You scan the documents that go inside it, and later you can search their text to find out which envelope holds the paper. Everything works offline. When you're online, the phone syncs with your own server.

```
mobile/    SDL3 app in C (iOS, Android, desktop for development). SQLite + FTS5, offline-first.
backend/   Node.js + Hapi + PostgreSQL: accounts, sync, scan storage, search, printable labels.
```

## How it works

| You do | The app does |
|---|---|
| **New file** → `F-0001` | Suggests the next number in your series. Print the QR label or write the number on the envelope. |
| **New file → Save & scan pages** | Saves the file, then opens the multi-page scanner straight away. |
| **Scan document** inside a file | Document camera with edge detection, then on-device OCR of each page. The title comes from the first line. All pages are saved as one PDF in the file (**View / share PDF** to open, print or save it). |
| **Search** "bescom" or "insurance 2026" | Full-text search (SQLite FTS5) over titles, tags, OCR text and file numbers. Each result shows **which file** the paper is in. |
| **Scan label** on an envelope | Reads the QR label, or a handwritten number with OCR, then opens that file. |
| Go online | Sends local changes and scans, then downloads changes and scans from your other devices. |

File numbers ignore case, spaces, punctuation and leading zeros, so `F-0042`, `f 42` and `F_042` all refer to the same file. The normalized number is the folder's ID on every device. If two phones create "F-42" while offline, the two records merge on sync instead of producing duplicates.

## Backend

Needs Node 20+ and PostgreSQL.

```sh
cd backend
npm install
createdb myfiles
DATABASE_URL=postgres://localhost/myfiles JWT_SECRET=$(openssl rand -hex 32) npm start   # migrates, listens on :4000
npm test        # integration tests against a throwaway Postgres database
```

| Endpoint | |
|---|---|
| `POST /api/auth/register`, `/api/auth/login` | `{email, password}` → `{token}` (JWT) |
| `GET /api/sync/pull?cursor=N` | Rows changed since cursor `N` (including deletions), in pages |
| `POST /api/sync/push` | Local changes; last write wins on `updated_at` |
| `PUT/GET /api/pages/:id/image` | Scan JPEGs (stored on disk under `BLOB_DIR`) |
| `GET /api/documents/:id/pdf` | The document's uploaded pages as one PDF (same layout as the phone's) |
| `GET /api/search?q=` | Postgres full-text search |
| `GET /api/labels/F-0042.svg`, `/api/labels/sheet.svg?prefix=F-&from=1&count=10` | Printable labels (no auth needed; they contain no private data) |

Environment variables: `PORT`, `DATABASE_URL`, `JWT_SECRET` (required in production), `BLOB_DIR`, `MAX_IMAGE_BYTES`.

## Mobile app

Structure: `src/db.c` (storage and search), `src/sync.c` (background sync thread), `src/ui.c` (a small touch UI on SDL_Renderer and SDL_ttf), `src/screens.c`, `src/label.c` (QR labels). Native features live in `src/platform/`:

| | Scanning | OCR | Label | HTTP |
|---|---|---|---|---|
| iOS | VisionKit document camera | Apple Vision (reads handwriting) | Vision QR, or OCR of a photo | NSURLSession |
| Android | ML Kit document scanner | ML Kit text recognition | ML Kit code scanner, or OCR of a photo | HttpURLConnection |
| Desktop (dev) | File picker | `tesseract` CLI if installed | OCR of a picked image | libcurl |

### Desktop (development)

```sh
brew install sdl3 sdl3_ttf sdl3_image   # optional: tesseract
cd mobile
cmake -S . -B build && cmake --build build -j
./build/test_core                                              # store/search tests
MYFILES_TEST_SERVER=http://127.0.0.1:4000 ./build/test_sync    # two-phone sync against a running backend
open build/MyFiles.app
```

Set `MYFILES_DATA_DIR=/some/dir` to use a throwaway data folder.

### iOS

```sh
cd mobile
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
open build-ios/MyFiles.xcodeproj    # set your signing team, then run on a device
```

To build and install on a connected iPhone from the command line:

```sh
cmake -S . -B build-iosdev -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DMYFILES_BUNDLE_ID=com.yourname.myfiles -DMYFILES_TEAM_ID=<your Apple team ID>
xcodebuild -project build-iosdev/MyFiles.xcodeproj -scheme MyFiles -configuration Release \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
xcrun devicectl list devices                     # find your phone's identifier
xcrun devicectl device install app --device <id> build-iosdev/Release-iphoneos/MyFiles.app
```

Your team ID is the `OU=` value of your "Apple Development" certificate
(`security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject`).

The first configure downloads and builds SDL3, SDL3_ttf and SDL3_image from source, which takes a few minutes. The simulator has no camera, so scanning falls back to the photo library there.

### Android

```sh
cd mobile/android
./fetch-sdl-java.sh        # already done once; re-run if you change the SDL version
# open this folder in Android Studio (or: gradle wrapper && ./gradlew installDebug)
```

Needs NDK 27 and CMake 3.31 from the SDK Manager. **Not built or run yet:** this machine has no Android SDK. The JNI C code has been type-checked, but the Kotlin and Gradle files haven't been compiled.

## Notes and limits

- Handwriting: Apple Vision reads handwritten file numbers well. ML Kit's on-device text recognition targets printed text, so on Android a printed QR label is the reliable option.
- The default server URL scheme allows plain `http` on the local network for development. Use HTTPS in production.
- Signing out keeps your documents on the phone. Signing in to a *different* account then uploads them into that account.
- Scans are stored on the server's disk. To move them to S3 or another object store, change `backend/src/routes/images.js`.
