// O blob da conta chega nos ajustes com o SENTIDO certo?
//
// Este e o teste que importa mais nesta area, porque o defeito aqui e MUDO:
// um mapeamento invertido nao da erro, nao trava, nao aparece no log — a
// pessoa so acha que a TV "veio com outras opcoes". Cada caso abaixo foi
// conferido contra o valor literal que o app web grava.
//
// Dois casos sao propositalmente traicoeiros:
//   - `true` do web vira INDICE 0 ("Ligado"), nao 1;
//   - `collapseSidebar: true` vira "Recolhida", que tambem e o indice 0.
#include <stdio.h>
#include <string.h>
#include "settings.h"

static int failures;
static void checks(const char *o_que, int got, int expected) {
  int ok = got == expected;
  printf("  %-46s %s (got %d, expected %d)\n", o_que, ok ? "ok    " : "FAILED", got, expected);
  if (!ok) failures++;
}

int main(void) {
  // Blob com TODO valor no oposto do padrao de fabrica, para que qualquer
  // opcao que nao seja aplicada apareca como falha em vez de coincidir.
  // FORMATO REAL, medido contra a conta na TV: version + features, chave em
  // snake_case, valor embrulhado em {"type","value"}. O teste antigo usava um
  // mapa plano de camelCase — passava, e o app nao aplicava NADA no aparelho.
  // Teste que nao usa o formato do servidor nao prova nada.
  static const char *BLOB =
    "{\"version\":1,\"features\":{\"layout_settings\":{"
    "\"modern_landscape_posters_enabled\":{\"type\":\"boolean\",\"value\":true},"
    "\"modern_hero_full_screen_backdrop_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"collapse_sidebar\":{\"type\":\"boolean\",\"value\":true},"
    "\"modern_sidebar\":{\"type\":\"boolean\",\"value\":false},"
    "\"hero_section_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"discover_location\":{\"type\":\"string\",\"value\":\"IN_SIDEBAR\"},"
    "\"poster_labels_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"hide_unreleased_content\":{\"type\":\"boolean\",\"value\":true},"
    "\"home_imdb_ratings_visibility\":{\"type\":\"string\",\"value\":\"HIDE_ALL\"},"
    "\"continue_watching_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"continue_watching_card_style\":{\"type\":\"string\",\"value\":\"POSTER\"},"
    "\"continue_watching_sort_mode\":{\"type\":\"string\",\"value\":\"streaming_style\"},"
    "\"blur_unwatched_episodes\":{\"type\":\"boolean\",\"value\":true},"
    "\"detail_page_trailer_button_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"focused_poster_backdrop_expand_delay_seconds\":{\"type\":\"int\",\"value\":4},"
    "\"card_depth_enabled\":{\"type\":\"boolean\",\"value\":false},"
    "\"card_depth_edge_strength\":{\"type\":\"int\",\"value\":80},"
    "\"poster_card_width_dp\":{\"type\":\"int\",\"value\":150},"
    "\"poster_card_corner_radius_dp\":{\"type\":\"int\",\"value\":12},"
    "\"a_key_this_app_does_not_know\":{\"type\":\"string\",\"value\":\"x\"}"
    "}}}";

  printf("applying the account blob...\n");
  settings_apply_blob(BLOB);

  printf("\nbooleans (true from the web = ON):\n");
  checks("modernLandscapePostersEnabled:true -> on", settings_posters_landscape(), 1);
  checks("modernHeroFullScreenBackdropEnabled:false",    settings_hero_full(), 0);
  checks("modernSidebar:false -> desligado",             settings_rail_modern(), 0);
  checks("heroSectionEnabled:false -> desligado",        settings_hero_on(), 0);
  checks("posterLabelsEnabled:false",                    settings_labels_poster(), 0);
  checks("hideUnreleasedContent:true",                   settings_hide_unreleased(), 1);
  checks("continueWatchingEnabled:false",                settings_cw_on(), 0);
  checks("blurUnwatchedEpisodes:true",                   settings_blur_unwatched(), 1);
  checks("detailPageTrailerButtonEnabled:false",         settings_button_trailer(), 0);
  checks("cardDepthEnabled:false",                       settings_depth(), 0);

  printf("\ncollapseSidebar:true = \"Collapsed\" (index 0):\n");
  checks("rail collapsed",                               settings_rail_collapsed(), 1);

  printf("\ntext enums:\n");
  checks("discoverLocation \"in_sidebar\" -> 1",         settings_local_discover(), 1);
  checks("homeImdbRatingsVisibility \"HIDE_ALL\"",       settings_scores_home(), 0);
  checks("continueWatchingCardStyle \"poster\" -> 2",    settings_cw_style(), 2);
  checks("continueWatchingSortMode \"streaming_style\"", settings_cw_order(), 1);

  printf("\nnumeros:\n");
  checks("focusedPosterBackdropExpandDelaySeconds 4",
          (int)(settings_expand_poster_delay() + 0.5f), 4);
  checks("cardDepthEdgeStrength 80 -> 0.80",
          (int)(settings_depth_border() * 100.0f + 0.5f), 80);
  checks("posterCardWidthDp 150",                        settings_width_poster_dp(), 150);
  checks("posterCardCornerRadiusDp 12",                  settings_radius_poster_dp(), 12);

  // Segunda rodada: valor de texto que este app NAO conhece (versao nova do
  // web, opcao nova). A opcao tem de FICAR COMO ESTA. Cair num padrao aqui
  // inventaria uma preferencia que a pessoa nunca marcou — e ela veria a TV
  // mudar sozinha sem ter mexido em nada.
  printf("\nan unknown value does not invent a preference:\n");
  { int beforeDiscover = settings_local_discover();
    int beforeStyle    = settings_cw_style();
    settings_apply_blob(
      "{\"features\":{\"layout_settings\":{"
      "\"discover_location\":{\"type\":\"string\",\"value\":\"a_mode_that_does_not_exist\"},"
      "\"continue_watching_card_style\":{\"type\":\"string\",\"value\":\"3d\"}}}}");
    checks("discoverLocation invalido -> mantido", settings_local_discover(), beforeDiscover);
    checks("cardStyle invalido -> mantido",        settings_cw_style(), beforeStyle); }

  // E chave ausente tambem nao mexe: um blob de uma opcao so nao pode zerar as
  // outras 39.
  printf("\na missing key does not disturb the rest:\n");
  { int before = settings_width_poster_dp();
    settings_apply_blob("{\"features\":{\"layout_settings\":{"
                         "\"hero_section_enabled\":{\"type\":\"boolean\",\"value\":true}}}}");
    checks("width preserved by a partial blob", settings_width_poster_dp(), before);
    checks("the key that arrived was applied",       settings_hero_on(), 1); }

  printf("\n%s\n", failures ? "HAS FAILURES" : "EVERY SETTING ARRIVED CORRECTLY");
  return failures ? 1 : 0;
}
