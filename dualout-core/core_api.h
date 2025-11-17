#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Вернёт JSON в out, без завершающего \0. Возвращает длину.
// Если буфер мал — вернёт требуемую длину (>outCap).
int dualout_core_list_devices_json(char* out, int outCap);

#ifdef __cplusplus
}
#endif
