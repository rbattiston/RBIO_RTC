#include "mesh_role.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "mesh_role";

#define NVS_NAMESPACE  "rbio_mesh"
#define NVS_KEY_ROLE   "role"

mesh_role_t mesh_role_get(void)
{
    nvs_handle_t h;
    uint8_t val = MESH_ROLE_ROOT;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_ROLE, &val);
        nvs_close(h);
    }
    if (val != MESH_ROLE_ROOT && val != MESH_ROLE_REPEATER) {
        val = MESH_ROLE_ROOT;
    }
    return (mesh_role_t)val;
}

esp_err_t mesh_role_set(mesh_role_t role)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, NVS_KEY_ROLE, (uint8_t)role);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Role set to %s (takes effect on reboot)", mesh_role_str(role));
    return ESP_OK;
}

const char *mesh_role_str(mesh_role_t role)
{
    switch (role) {
    case MESH_ROLE_ROOT:     return "ROOT";
    case MESH_ROLE_REPEATER: return "REPEATER";
    default:                 return "?";
    }
}
