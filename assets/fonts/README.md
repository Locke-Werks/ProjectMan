# Bundled fonts

The ARCHON / Specter Point design language specifies Chakra Petch for headings
and labels and Outfit for body text. Neither is installed on Windows, and the
design system's CSS pulls both from Google Fonts, which a Qt application cannot
do. They are vendored here and loaded at startup with
`QFontDatabase::addApplicationFont`.

Both are licensed under the SIL Open Font License 1.1. The license text ships
alongside each, as the OFL requires.

- Chakra Petch, Cadson Demak. `OFL-ChakraPetch.txt`
- Outfit, Smartsheet Inc. and Rodrigo Fuenzalida. `OFL-Outfit.txt`

Outfit is the variable weight axis file, which covers 300 to 700 from one face.
