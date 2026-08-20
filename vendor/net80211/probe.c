/* Pull esp_wifi_80211_tx and see what the linker demands behind it. */
extern int esp_wifi_80211_tx(int ifx, const void *buf, int len, int seq);
int probe_entry(void);
int probe_entry(void) { static char f[64]; return esp_wifi_80211_tx(0, f, sizeof f, 1); }
