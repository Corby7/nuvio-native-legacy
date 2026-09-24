// Settings screen: a list of sections; opening one shows its options, label on
// the left and value on the right.
//
// The LAYOUT keys are the same as the web app's
// js/data/local/layoutPreferences.js, with the same names, the same factory
// defaults and — where the port draws the screen — the same effect. They are
// not preferences invented for the port: the owner already changes them in the
// web Settings screen, and their home depends on them.
//
// The values are written to <dir>/settings.txt, one key per line.
#ifndef NV_SETTINGS_H
#define NV_SETTINGS_H
#include <SDL2/SDL.h>

int  settings_start(void);
// COMING BACK to the screen within a few minutes: what the viewer left — the
// typed text, the results, the focus and the scroll — stays, and only the
// requests the last visit left pending are cleared. app.c decides which of the
// two a visit is (swapScreen).
void settings_resume(void);

// Folder the settings are read from and written to. Call once, at startup.
void settings_dir(const char *dir);
void settings_event(const SDL_Event *e);
void settings_update(float dt, Uint32 now);
void settings_draw(Uint32 now);
int  settings_wants_exit(void);   // 1 when Back on the list of sections closes the screen
int  settings_requested_menu(void);   // LEFT on the list of sections: calls up the menu
void settings_shutdown(void);

// Read by the rest of the app. "Reduced animations" matters most: with it on,
// anything that animates should jump straight to the target instead of calling
// anim_spring — it is an accessibility setting, not a taste, and a screen that
// ignores it is no use to the person who turned it on.
int settings_animations_reduced(void);
int settings_dolby_vision(void);
int settings_dolby_atmos(void);
// Which subtitle the player turns on by itself: 0 off, 1 automatic (English),
// 2 Portuguese, 3 English — the file's own track first, a download after. Read by
// tracks.c, which owns the selection.
int settings_subtitle_pref(void);
// The same row as a lang.h index: -1 for Off, English for Automatic.
int settings_subtitle_language(void);
// 1 when a forced track in the audio's language should come on whenever the
// automatic selection turns no other subtitle on.
int settings_subtitle_forced(void);
// The preferred AUDIO language as a lang.h index, -1 for the file's default.
int settings_audio_language(void);
// 1 when the Up next countdown plays the next episode on its own.
int settings_next_autoplay(void);
// Seconds before the end at which the Up next card appears, for an episode of
// `durationSeg` — from the fixed lead or the share-watched row, whichever is on.
double settings_next_lead(double durationSeg);
// The player's seek bar colour (fill and playhead), from the Playback row.
void settings_seek_color(float *r, float *g, float *b);
// Seconds the Up next card counts down before it plays the next episode.
int settings_next_countdown(void);
// "Automatic", "4K", "1080p" or "720p" — the displayed label, so whatever picks
// the video source shows exactly what the user chose.
const char *settings_quality(void);

// --- LAYOUT: home structure -------------------------------------------------
int   settings_rail_collapsed(void);     // collapseSidebar
int   settings_rail_modern(void);       // modernSidebar
int   settings_rail_modern_blur(void);  // modernSidebarBlur
int   settings_hero_on(void);        // heroSectionEnabled
int   settings_hero_full(void);         // modernHeroFullScreenBackdropEnabled
// heroBackdropArea: with the full-screen backdrop on, whether it covers the whole
// screen (0) or is drawn whole, at its own aspect, in a band at the top-right (1).
// Local to this port — the web app has no equivalent key, so the account blob
// never touches it. Answers 0 whenever the full-screen backdrop is off.
int   settings_hero_top_band(void);     // heroBackdropArea == "Top band"
// heroBackdropScale / 100: the band's width as a fraction of the screen's. The
// height comes from the art's own aspect, so this is the whole of its size.
float settings_hero_band_scale(void);   // heroBackdropScale
int   settings_posters_landscape(void);  // modernLandscapePostersEnabled
int   settings_gradient_focus_classic(void); // classicFocusGradientEnabled
// socialRowEnabled: the "Among friends" row on Home. Local to this port — the
// web app has no equivalent key, so the account blob never touches it.
int   settings_social_row(void);
// The x where the content starts. Not a constant: the inset is always 104 and
// the rail adds its own 144 when it is fixed.
float settings_content_x(void);

// --- LAYOUT: labels and metadata --------------------------------------------
int   settings_labels_poster(void);     // posterLabelsEnabled
int   settings_name_addon(void);         // catalogAddonNameEnabled
int   settings_suffix_kind(void);        // catalogTypeSuffixEnabled
int   settings_hide_unreleased(void);   // hideUnreleasedContent
int   settings_date_full(void);      // showFullReleaseDate
// homeImdbRatingsVisibility: 0 SHOW_ALL, 1 HIDE_ALL
int   settings_scores_home(void);
// discoverLocation: 0 in_search, 1 in_sidebar, 2 off
int   settings_local_discover(void);
int   settings_discover_na_search(void); // searchDiscoverEnabled (derived)

// --- LAYOUT: continue watching ----------------------------------------------
int   settings_cw_on(void);          // continueWatchingEnabled
int   settings_cw_style(void);          // 0 card, 1 wide, 2 poster
int   settings_cw_logo(void);           // local: logo in place of the title
int   settings_cw_play(void);           // local: OK on a resume card plays it
int   settings_cw_thumb_episode(void);  // useEpisodeThumbnailsInCw
int   settings_cw_blur_next(void);// blurContinueWatchingNextUp
int   settings_cw_do_episode_more_alto(void); // nextUpFromFurthestEpisode
int   settings_cw_show_unaired(void);  // showUnairedNextUp
// continueWatchingSortMode: 0 default, 1 streaming_style, 2 split_upcoming
int   settings_cw_order(void);

// --- LAYOUT: detail page (the effect lives in detail.c) ----------------------
int   settings_blur_unwatched(void); // blurUnwatchedEpisodes
int   settings_button_trailer(void);           // detailPageTrailerButtonEnabled
int   settings_meta_external(void);            // preferExternalMetaAddonDetail

// --- LAYOUT: poster focus ----------------------------------------------------
int   settings_expand_poster(void);         // focusedPosterBackdropExpandEnabled
float settings_expand_poster_delay(void);  // in seconds
int   settings_navigation_horizontal_fast(void); // fastHorizontalNavigationEnabled

// --- LAYOUT: card depth ------------------------------------------------------
int   settings_depth(void);            // cardDepthEnabled
float settings_depth_border(void);      // 0..1 (cardDepthEdgeStrength/100)
float settings_depth_brightness(void);     // 0..1 (cardDepthSheenStrength/100)
float settings_depth_coverage(void);  // 0..1 (cardDepthEdgeCoverage/100)
int   settings_depth_posters(void);
int   settings_depth_cw(void);
int   settings_depth_episodes(void);
int   settings_depth_cast(void);
int   settings_depth_trailers(void);

// --- LAYOUT: item size -------------------------------------------------------
// `posterCardWidthDp` DOES set the poster's size in the modern layout. This said
// the opposite until 2026-09-07, on the grounds that
// `.home-screen-shell.home-layout-modern` redefines `--home-poster-width` to 212px
// (components.css:6662) — it does, and it loses: `buildModernHomeSizingStyle`
// writes the variable in the shell's STYLE ATTRIBUTE, and an inline custom property
// beats a stylesheet. Read off the running app at 126 dp:
//
//   style="--home-poster-width:229px;--home-poster-height:343px;
//          --home-landscape-poster-width:419px;--home-landscape-poster-height:237px;
//          --home-poster-radius:24px"
//
// and `.home-poster-card` measures 229 x 347 (the frame plus its 2px border).
// NV_CARD_W/H carry those numbers for the DEFAULT 126 dp; the preference itself is
// still not wired to them here, so a profile on another width will differ.
int   settings_width_poster_dp(void);
int   settings_radius_poster_dp(void);
float settings_radius_poster_px(void);   // radius in px (dp x 2)

// --- SETTINGS THAT COME FROM THE ACCOUNT -------------------------------------
// Applies the blob from `sync_pull_profile_settings_blob` (the `settings_json`
// object, as raw JSON text) over the local values. Returns how many options
// changed.
//
// WHY THIS EXISTS: the ~40 keys in this file (`heroSectionEnabled`,
// `continueWatchingCardStyle`, `cardDepthEnabled`, `posterCardWidthDp`...) are
// the SAME as the web app's, and the defaults here were transcribed by hand
// from the profile of whoever built the package. Without applying the blob,
// whoever installs it gets somebody else's layout instead of their own — the
// same defect art/addons.txt had.
//
// A key missing from the blob does NOT touch the option, and neither does a
// text value this app does not recognise: substituting a default would be
// inventing a choice the user never made.
int settings_apply_blob(const char *json);

#endif
