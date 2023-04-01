#include "../bc_scanner_app_i.h"
#include <assets_icons.h>

typedef enum {
    BarCodeCustomEventErrorBack,
} BarCodeCustomEvent;

static void
    bc_scanner_scene_error_event_callback(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    BarCodeApp* app = context;

    if((result == GuiButtonTypeLeft) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(app->view_dispatcher, BarCodeCustomEventErrorBack);
    }
}

void bc_scanner_scene_error_on_enter(void* context) {
    BarCodeApp* app = context;

    if(app->error == BarCodeAppErrorNoFiles) {
        widget_add_icon_element(app->widget, 0, 0, &I_SDQuestion_35x43);
        widget_add_string_multiline_element(
            app->widget,
            81,
            4,
            AlignCenter,
            AlignTop,
            FontSecondary,
            "No SD card or\napp data found.\nThis app will not\nwork without\nrequired files.");
        widget_add_button_element(
            app->widget, GuiButtonTypeLeft, "Back", bc_scanner_scene_error_event_callback, app);
    } else if(app->error == BarCodeAppErrorCloseRpc) {
        widget_add_icon_element(app->widget, 78, 0, &I_ActiveConnection_50x64);
        widget_add_string_multiline_element(
            app->widget, 3, 2, AlignLeft, AlignTop, FontPrimary, "Connection\nis active!");
        widget_add_string_multiline_element(
            app->widget,
            3,
            30,
            AlignLeft,
            AlignTop,
            FontSecondary,
            "Disconnect from\nPC or phone to\nuse this function.");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BarCodeAppViewError);
}

bool bc_scanner_scene_error_on_event(void* context, SceneManagerEvent event) {
    BarCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BarCodeCustomEventErrorBack) {
            view_dispatcher_stop(app->view_dispatcher);
            consumed = true;
        }
    }
    return consumed;
}

void bc_scanner_scene_error_on_exit(void* context) {
    BarCodeApp* app = context;
    widget_reset(app->widget);
}
