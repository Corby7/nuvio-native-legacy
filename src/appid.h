#pragma once
// The app's identity, in one place. The id is what webOS installs under, so it
// has to differ from every other Nuvio build (upstream's space.nuvio.native.legacy,
// the web space.nuvio.webos) for this one to install alongside them. Both values
// must match deploy/app/appinfo.json: the device has no way to read the manifest
// at runtime. The version is not repeated here: tools/env.sh reads it from
// appinfo.json and passes it as -DNV_APP_VERSION, so a release bumps one file.
#define NV_APP_ID      "io.github.corby7.nuvio"
#ifndef NV_APP_VERSION
#define NV_APP_VERSION "dev"
#endif
