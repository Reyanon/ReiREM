// Find supported games in ROM folders (shared by Vita/PSP frontends, docs/DECISIONS.md ADR-016)
#ifndef REI_GAMES_H
#define REI_GAMES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int game;          // rei_core_game_* index
	char zip[160];     // <set>.zip path
	char parent[160];  // parent zip path for clones (empty if not found — split sets may then fail to run)
	int present;       // ROM zip exists (if not, zip is empty: greyed out in the list)
	int supported;     // board runnable on this platform (scan sets 1, PSP keeps only M92)
} rei_game_found;

// Fill out with all supported games, newest board first (M107, M92, ..., M57, M52; same board in driver table order), return the count
// (at most max). Each game's "<set>.zip" is looked up in dirs order to set present (first folder found per game, names
// case-insensitive). Parent zip is searched in the same folder first, then the others. Up to 16 folders. *npresent = games with ROMs
#define REI_GAMES_MAX 256
int rei_games_scan(const char *const *dirs, int ndirs, rei_game_found *out, int max, int *npresent);
// Sort index of a board name ("M92") (numeric part, large value if none)
int rei_board_order(const char *board);

#ifdef __cplusplus
}
#endif

#endif
