// Turns a downloaded subtitle into UTF-8, whatever it was written in.
//
// Addons serve plenty of subtitles that are not UTF-8 — OpenSubtitles and SubDL
// especially hand out Windows-1250/1251/1252/1256 files — and the overlay's text
// renderer only reads UTF-8. Before this, every accented letter in such a file
// broke the line, and a Czech, Russian or Arabic subtitle was unreadable.
//
// The order of trust is the web app's (js/core/player/subtitleCharsetDetector.js):
// a BOM, then a strict UTF-8 check, then the subtitle's language, then a
// byte-frequency guess. Only single-byte codepages are decoded. The multibyte CJK
// ones (GB18030, Big5, Shift_JIS, EUC-KR) would need tables of tens of thousands
// of entries, so a CJK file that is not UTF-8 is refused instead of shown as
// garbage.
#ifndef NV_SUBCHARSET_H
#define NV_SUBCHARSET_H

// Decodes `n` bytes into a fresh NUL-terminated UTF-8 string; the caller frees
// it. `language` is the addon's code for the subtitle ("cze", "pt-br", "" when
// unknown). `*charset`, when not NULL, receives the name of what was decoded, for
// the log. Returns NULL for an empty body, an unsupported charset or no memory.
char *subcharset_utf8(const char *bytes, long n, const char *language,
                      const char **charset);

#endif
