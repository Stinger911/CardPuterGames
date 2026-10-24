#pragma once
#include <string>

#include "cJSON.h"

// RAII-владелец дерева cJSON
struct JDoc {
  cJSON* p;
  explicit JDoc(cJSON* j = nullptr) : p(j) {}
  ~JDoc() { cJSON_Delete(p); }
  JDoc(const JDoc&) = delete;
  JDoc& operator=(const JDoc&) = delete;
};

inline std::string jdump(const cJSON* j) {
  char* s = cJSON_PrintUnformatted(j);
  std::string r = s ? s : "";
  cJSON_free(s);
  return r;
}

inline std::string jstr(const cJSON* o, const char* k, const char* def = "") {
  const cJSON* i = cJSON_GetObjectItemCaseSensitive(o, k);
  return (cJSON_IsString(i) && i->valuestring) ? i->valuestring : def;
}

inline int jint(const cJSON* o, const char* k, int def = 0) {
  const cJSON* i = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsNumber(i) ? i->valueint : def;
}

inline bool jbool(const cJSON* o, const char* k, bool def) {
  const cJSON* i = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsBool(i) ? cJSON_IsTrue(i) : def;
}
