# docs/vendor

Manufacturer-supplied material for the HS-S62A-PL module — datasheets, archives
and example projects for other boards — lives here on the development machine
but is **deliberately not committed**. It is roughly 150 MB (a 70 MB zip, a
33 MB rar, five PDFs, 61 PNGs and an Android APK), none of which the firmware
build needs, and most of which is not ours to redistribute.

One subdirectory is kept under version control on purpose:

```
YFROBOTRFIDI2C-1.0.3-original/    pristine upstream copy of the vendor driver
```

`lib/YFROBOTRFIDI2C/` ships a **patched** version of that driver, and
`lib/YFROBOTRFIDI2C/PATCHES.md` documents every change. Keeping the untouched
original next to it makes those patches diffable and makes a future library
upgrade auditable — which is the whole reason it is here rather than ignored
along with the rest.

If you need the full manufacturer package, request it from YFROBOT
(<https://www.yfrobot.com.cn>). Please do not vendor it back into this
repository.
