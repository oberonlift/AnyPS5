# Next steps

## 1. Run relinked PS5 executables

- Seed discovery from `DT_INIT_ARRAY` and `DT_FINI_ARRAY`.
- Handle TLS relocations beyond static TLS.
- Lift the bundled `sce_module` files the relinker converts, and link the bridges for every library a title imports.
- Bridge the exports still left out: records passed or returned by value, complex values, function-pointer returns and C++-linkage exports.

## 2. Breakout demo, then one title

Neither is in hand yet: the demo would be homebrew built with an open PS5 SDK, which the repo can carry, and a title is a dump that cannot be. Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
