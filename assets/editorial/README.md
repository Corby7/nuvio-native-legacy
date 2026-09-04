# Collection editorial artwork

23 identities, each with a separate home and detail composition. Sources are
editable SVGs; no lettering is baked into the illustrations. The application
renders one title in the empty left column.

Home: 3840×1000 → 1920×500. Detail: 3840×640 → 1920×320. Both dissolve to
`#0d0d0d` before content begins. Do not render these using a backdrop/crop shader
or extend them to full-screen: the shelves and item panel must stay neutral.

Generate with Node and `sharp` installed (or available through `NODE_PATH`):

```sh
node tools/build-editorial-art.mjs
node tests/editorial-art.mjs
```

The manifest contains public identities only. The generator reads collection
IDs but never exports catalog source URLs. PNGs live in `deploy/app/art/editorial`;
the loader enables artwork only when both files exist. Missing pairs retain the
existing collection fallback. Runtime decoding is capped by the hero texture
cache, independent of master resolution.
