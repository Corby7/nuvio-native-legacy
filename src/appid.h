#pragma once
// The app's identity, in one place. The id is what webOS installs under, so it
// has to differ from every other Nuvio build (upstream's space.nuvio.native.legacy,
// the web space.nuvio.webos) for this one to install alongside them. Both values
// must match deploy/app/appinfo.json: the device has no way to read the manifest
// at runtime.
#define NV_APP_ID      "io.github.corby7.nuvio"
#define NV_APP_VERSION "1.0.0"
