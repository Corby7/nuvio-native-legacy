// Settings screen: a vertical list of options in sections, label on the left
// and value on the right.
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

// Folder the settings are read from and written to. Call once, at startup.
void settings_dir(const char *dir);
void settings_event(const SDL_Event *e);
void settings_update(float dt, Uint32 now);
void settings_draw(Uint32 now);
int  settings_wants_exit(void);   // 1 quando o Back deve fechar a tela
void settings_shutdown(void);

// Read by the rest of the app. "Reduced animations" matters most: with it on,
// anything that animates should jump straight to the target instead of calling
// anim_spring — it is an accessibility setting, not a taste, and a screen that
// ignores it is no use to the person who turned it on.
int settings_animations_reduced(void);
int settings_dolby_vision(void);
int settings_dolby_atmos(void);
// "Automatic", "4K", "1080p" or "720p" — the displayed label, so whatever picks
// the video source shows exactly what the user chose.
const char *settings_quality(void);

// --- LAYOUT: estrutura da home ----------------------------------------------
int   settings_rail_collapsed(void);     // collapseSidebar
int   settings_rail_modern(void);       // modernSidebar
int   settings_rail_modern_blur(void);  // modernSidebarBlur
int   settings_hero_on(void);        // heroSectionEnabled
int   settings_hero_full(void);         // modernHeroFullScreenBackdropEnabled
int   settings_posters_landscape(void);  // modernLandscapePostersEnabled
int   settings_gradient_focus_classic(void); // classicFocusGradientEnabled
// The x where the content starts. Not a constant: the inset is always 104 and
// the rail adds its own 144 when it is fixed.
float settings_content_x(void);

// --- LAYOUT: rotulos e metadados --------------------------------------------
int   settings_labels_poster(void);     // posterLabelsEnabled
int   settings_name_addon(void);         // catalogAddonNameEnabled
int   settings_suffix_kind(void);        // catalogTypeSuffixEnabled
int   settings_hide_unreleased(void);   // hideUnreleasedContent
int   settings_date_full(void);      // showFullReleaseDate
// homeImdbRatingsVisibility: 0 SHOW_ALL, 1 HIDE_ALL
int   settings_scores_home(void);
// discoverLocation: 0 in_search, 1 in_sidebar, 2 off
int   settings_local_discover(void);
int   settings_discover_na_search(void); // searchDiscoverEnabled (derivado)

// --- LAYOUT: continuar assistindo -------------------------------------------
int   settings_cw_on(void);          // continueWatchingEnabled
int   settings_cw_style(void);          // 0 card, 1 largo (wide), 2 poster
int   settings_cw_thumb_episode(void);  // useEpisodeThumbnailsInCw
int   settings_cw_blur_next(void);// blurContinueWatchingNextUp
int   settings_cw_do_episode_more_alto(void); // nextUpFromFurthestEpisode
int   settings_cw_show_unaired(void);  // showUnairedNextUp
// continueWatchingSortMode: 0 default, 1 streaming_style, 2 split_upcoming
int   settings_cw_order(void);

// --- LAYOUT: pagina de detalhe (efeito vive em detail.c) ---------------------
int   settings_blur_unwatched(void); // blurUnwatchedEpisodes
int   settings_button_trailer(void);           // detailPageTrailerButtonEnabled
int   settings_meta_external(void);            // preferExternalMetaAddonDetail

// --- LAYOUT: foco no poster --------------------------------------------------
int   settings_expand_poster(void);         // focusedPosterBackdropExpandEnabled
float settings_expand_poster_delay(void);  // em segundos
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
// CAREFUL, this is the trap that already cost one wrong measurement: in the
// MODERN layout `posterCardWidthDp` does NOT change the poster size.
// `buildModernHomeSizingStyle` produces --home-poster-width: 218px for 120dp,
// but the modern layout's stylesheet redefines the variable to 212px in
// .home-screen-shell.home-layout-modern (components.css:6462) and that is the
// one that wins — CHECKED in the running app: changing the inline variable from
// 218 to 300 did not move the card by a pixel. All that comes out of the
// preference is the RADIUS.
int   settings_width_poster_dp(void);
int   settings_radius_poster_dp(void);
float settings_radius_poster_px(void);   // raio em px (dp x 2)

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
