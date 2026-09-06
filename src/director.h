// A short profile of a director for the detail screen and filmography: photo,
// bio, birth and the titles they are known for. All from TMDB, starting from
// the NAME (the collection only has the name). Does not block: requesting is
// asking, frame by frame. The profile is only published once TMDB has
// confirmed both the name and the director identity.
#ifndef NV_DIRECTOR_H
#define NV_DIRECTOR_H
void director_request(const char *name);        // caches; failures retry with backoff
int  director_ready(const char *name);       // 1 once the record has arrived
const char *director_photo(const char *name);  // URL original, ou ""
const char *director_hero(const char *name);  // original backdrop of a known work
const char *director_bio(const char *name);
const char *director_meta(const char *name);  // "Director · Born in ... · Place"
const char *director_known(const char *name); // "A · B · C"
#endif
