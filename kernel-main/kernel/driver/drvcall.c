#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/drvcall.h>
#include <kernel/driver/kdrv_loader.h>

#include <kernel/util/string.h>

//
// Driver types (driver.h)
//

const char* driver_type_name(uint32_t type) {
    switch (type) {
        case DRIVER_TYPE_DEVICE:  return "device";
        case DRIVER_TYPE_LIBRARY: return "library";
        case DRIVER_TYPE_SERVICE: return "service";
        default:                  return "unknown";
    }
}

static bool text_fits(const char* text, uint32_t max) {
    return text != NULL && strnlen(text, max) < max;
}

static bool export_name_ok(const char* name) {
    if (!text_fits(name, DRIVER_EXPORT_NAME_MAX) || name[0] == '\0')
        return false;
    for (const char* c = name; *c != '\0'; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
        if (!ok)
            return false;
    }
    return true;
}

static bool export_params_ok(const char* params) {
    if (!text_fits(params, DRIVER_CALL_ARGS_MAX + 1))
        return false;
    for (const char* c = params; *c != '\0'; c++) {
        if (*c != DRIVER_ARG_INT && *c != DRIVER_ARG_STRING)
            return false;
    }
    return true;
}

static bool exports_ok(const struct DriverExport* exports) {
    if (exports == NULL || exports[0].name == NULL)
        return false;   // A library with nothing in it
    for (uint32_t i = 0; exports[i].name != NULL; i++) {
        if (i >= DRIVER_EXPORT_MAX)
            return false;
        const struct DriverExport* row = &exports[i];
        if (!export_name_ok(row->name) || !export_params_ok(row->params) || row->fn == NULL)
            return false;
        if (row->help != NULL && !text_fits(row->help, DRIVER_EXPORT_HELP_MAX))
            return false;
        for (uint32_t j = 0; j < i; j++) {
            if (strcmp(exports[j].name, row->name) == 0)
                return false;   // Two functions of one name: which would a call get?
        }
    }
    return true;
}

bool driver_shape_ok(const struct Driver* drv) {
    if (drv == NULL)
        return false;
    switch (drv->type) {
        case DRIVER_TYPE_DEVICE:
            return drv->match != NULL && drv->probe != NULL && drv->exports == NULL;
        case DRIVER_TYPE_LIBRARY:
            return drv->match == NULL && drv->probe == NULL && drv->remove == NULL &&
                   exports_ok(drv->exports);
        case DRIVER_TYPE_SERVICE:
            return drv->match == NULL && drv->probe == NULL && drv->remove == NULL &&
                   drv->exports == NULL && drv->poll != NULL;
        default:
            return false;
    }
}

//
// Calls
//

const char* drvcall_error_string(DrvCallResult result) {
    switch (result) {
        case DRVCALL_OK:          return "ok";
        case DRVCALL_NO_DRIVER:   return "no driver of that name is loaded";
        case DRVCALL_NOT_LIBRARY: return "not a library driver (it exports no functions)";
        case DRVCALL_NO_FUNCTION: return "the driver has no function of that name";
        case DRVCALL_BAD_ARGS:    return "wrong number or type of arguments";
        case DRVCALL_DENIED:      return "permission denied";
    }
    return "error";
}

const struct DriverExport* driver_export_find(const struct Driver* drv, const char* function) {
    if (drv == NULL || drv->type != DRIVER_TYPE_LIBRARY || drv->exports == NULL || function == NULL)
        return NULL;
    for (const struct DriverExport* row = drv->exports; row->name != NULL; row++) {
        if (strncmp(row->name, function, DRIVER_EXPORT_NAME_MAX) == 0)
            return row;
    }
    return NULL;
}

// The library, held so it can't be unloaded until kdrv_release
static DrvCallResult hold_library(const char* driver, const struct Driver** out) {
    const struct Driver* drv = (driver != NULL) ? kdrv_hold(driver) : NULL;
    if (drv == NULL)
        return DRVCALL_NO_DRIVER;
    if (drv->type != DRIVER_TYPE_LIBRARY) {
        kdrv_release(drv);
        return DRVCALL_NOT_LIBRARY;
    }
    *out = drv;
    return DRVCALL_OK;
}

static bool args_fit(const struct DriverExport* fn, const struct DriverCall* call) {
    uint32_t count = (uint32_t)strlen(fn->params);
    if (call->argc != count || call->argc > DRIVER_CALL_ARGS_MAX)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const struct DriverArg* arg = &call->args[i];
        if (arg->type != fn->params[i])
            return false;
        if (arg->type == DRIVER_ARG_STRING && !text_fits(arg->s, DRIVER_CALL_STRING_MAX))
            return false;
    }
    return true;
}

DrvCallResult driver_call(const char* driver, const char* function, struct DriverCall* call,
                          uint32_t clearance, int32_t* result) {
    if (call == NULL || (call->out == NULL && call->out_size != 0))
        return DRVCALL_BAD_ARGS;

    const struct Driver* drv;
    DrvCallResult status = hold_library(driver, &drv);
    if (status != DRVCALL_OK)
        return status;

    const struct DriverExport* fn = driver_export_find(drv, function);
    if (fn == NULL) {
        status = DRVCALL_NO_FUNCTION;
    } else if ((fn->security & ~clearance) != 0) {
        status = DRVCALL_DENIED;
    } else if (!args_fit(fn, call)) {
        status = DRVCALL_BAD_ARGS;
    } else {
        // Unused argument slots read as zero, never as the caller's leftovers
        for (uint32_t i = call->argc; i < DRIVER_CALL_ARGS_MAX; i++)
            memset(&call->args[i], 0, sizeof(call->args[i]));
        if (call->out_size != 0)
            call->out[0] = '\0';
        call->out_length = 0;

        int32_t value = fn->fn(call);

        if (call->out_size != 0) {
            call->out[call->out_size - 1] = '\0';
            call->out_length = (uint32_t)strlen(call->out);
        }
        if (result != NULL)
            *result = value;
    }

    kdrv_release(drv);
    return status;
}

static void copy_text(char* dst, const char* src, uint32_t size) {
    if (src == NULL)
        src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

DrvCallResult driver_export_info(const char* driver, uint32_t index, DrvExportInfo* info) {
    if (info == NULL)
        return DRVCALL_BAD_ARGS;

    const struct Driver* drv;
    DrvCallResult status = hold_library(driver, &drv);
    if (status != DRVCALL_OK)
        return status;

    status = DRVCALL_NO_FUNCTION;
    for (uint32_t i = 0; drv->exports[i].name != NULL; i++) {
        if (i != index)
            continue;
        const struct DriverExport* row = &drv->exports[i];
        memset(info, 0, sizeof(*info));
        copy_text(info->name,   row->name,   sizeof(info->name));
        copy_text(info->params, row->params, sizeof(info->params));
        copy_text(info->help,   row->help,   sizeof(info->help));
        info->security = row->security;
        status = DRVCALL_OK;
        break;
    }

    kdrv_release(drv);
    return status;
}
