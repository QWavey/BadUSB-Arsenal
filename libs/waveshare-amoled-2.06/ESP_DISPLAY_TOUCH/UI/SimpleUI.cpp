#include "SimpleUI.h"

// ==================== Implementation ====================

void SimpleUI_init() {}

void SimpleUI_update() {
    lv_timer_handler();
}

UIScreen::UIScreen() : screen(nullptr) {}

UIScreen::~UIScreen() {
    if (screen && lv_obj_is_valid(screen)) {
        lv_obj_delete(screen);
    }
}

void UIScreen::load() {
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x1a1a1a), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_screen_load(screen);
}

void UIScreen::setBackgroundColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!screen) {
        screen = lv_obj_create(NULL);
    }
    uint32_t color = (r << 16) | (g << 8) | b;
    lv_obj_set_style_bg_color(screen, lv_color_hex(color), 0);
}

UILabel::UILabel() : label(nullptr) {}

UILabel::~UILabel() {
    if (label && lv_obj_is_valid(label)) {
        lv_obj_delete(label);
    }
}

void UILabel::create(lv_obj_t* parent, int x, int y, int w) {
    if (!parent) parent = lv_screen_active();
    
    label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    
    if (w != LV_SIZE_CONTENT) {
        lv_obj_set_width(label, w);
    }
    
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_label_set_text(label, "Label");
}

void UILabel::setText(const char* text) {
    if (label) {
        lv_label_set_text(label, text);
    }
}

void UILabel::setColor(uint8_t r, uint8_t g, uint8_t b) {
    if (label) {
        uint32_t color = (r << 16) | (g << 8) | b;
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    }
}

void UILabel::setFontSize(uint8_t size) {
    if (label) {
        if (size <= 12) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        } else if (size <= 14) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        } else if (size <= 16) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        } else if (size <= 20) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        } else {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        }
    }
}

void UILabel::hide() {
    if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
}

void UILabel::show() {
    if (label) lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

UIButton::UIButton() : btn(nullptr), label(nullptr) {}

UIButton::~UIButton() {
    if (btn && lv_obj_is_valid(btn)) {
        lv_obj_delete(btn);
    }
}

void UIButton::create(lv_obj_t* parent, const char* text, int x, int y, int w, int h) {
    if (!parent) parent = lv_screen_active();
    
    btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_pad_all(btn, 10, 0);
    
    label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);

    // Register the click handler once. onClick() only stores the callback.
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, this);
}

void UIButton::onClick(std::function<void()> callback) {
    clickCallback = callback;
}

void UIButton::event_handler(lv_event_t* e) {
    UIButton* instance = (UIButton*)lv_event_get_user_data(e);
    if (instance && instance->clickCallback) {
        instance->clickCallback();
    }
}

void UIButton::setBackgroundColor(uint8_t r, uint8_t g, uint8_t b) {
    if (btn) {
        uint32_t color = (r << 16) | (g << 8) | b;
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    }
}

void UIButton::setText(const char* text) {
    if (label) {
        lv_label_set_text(label, text);
    }
}

void UIButton::hide() {
    if (btn) lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

void UIButton::show() {
    if (btn) lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

void UIButton::enable() {
    if (btn) lv_obj_clear_state(btn, LV_STATE_DISABLED);
}

void UIButton::disable() {
    if (btn) lv_obj_add_state(btn, LV_STATE_DISABLED);
}

UITextBox::UITextBox() : textarea(nullptr), linkedKeyboard(nullptr) {}

UITextBox::~UITextBox() {
    // Detach from the keyboard before this box goes away, so the keyboard never holds a dead pointer
    if (linkedKeyboard) {
        linkedKeyboard->unlinkTextBox(this);
        linkedKeyboard = nullptr;
    }
    if (textarea && lv_obj_is_valid(textarea)) {
        lv_obj_delete(textarea);
    }
}

void UITextBox::create(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!parent) parent = lv_screen_active();
    
    textarea = lv_textarea_create(parent);
    lv_obj_set_pos(textarea, x, y);
    lv_obj_set_size(textarea, w, h);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, 64);
    
    lv_obj_set_style_radius(textarea, 8, 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(textarea, 2, 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_all(textarea, 8, 0);
    
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x2196F3), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(textarea, 3, LV_STATE_FOCUSED);
    
    lv_obj_set_style_border_color(textarea, lv_color_hex(0xffffff), LV_PART_CURSOR);
    lv_obj_set_style_border_width(textarea, 2, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0xffffff), LV_PART_CURSOR);
    
    lv_obj_set_style_anim_duration(textarea, 500, LV_PART_CURSOR);
    
    lv_obj_add_event_cb(textarea, event_handler, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(textarea, event_handler, LV_EVENT_READY, this);
    lv_obj_add_event_cb(textarea, event_handler, LV_EVENT_DELETE, this);
}

void UITextBox::event_handler(lv_event_t* e) {
    UITextBox* instance = (UITextBox*)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        Serial.println("TextBox: Clicked");
        if (instance && instance->clickedCallback) {
            instance->clickedCallback();
        }
    }
    else if (code == LV_EVENT_READY) {
        Serial.println("TextBox: Ready (Enter pressed)");
        if (instance) {
            instance->submitText();
        }
    }
    else if (code == LV_EVENT_DELETE) {
        // The LVGL object is going away (this box is being destroyed, or its parent was deleted).
        // Drop the keyboard link and forget the object, so nothing uses it afterwards.
        if (instance) {
            if (instance->linkedKeyboard) {
                instance->linkedKeyboard->unlinkTextBox(instance);
                instance->linkedKeyboard = nullptr;
            }
            instance->textarea = nullptr;
        }
    }
}

void UITextBox::submitText() {
    if (submitCallback) {
        submitCallback(getText());
    }
}

void UITextBox::setPlaceholder(const char* text) {
    if (textarea) {
        lv_textarea_set_placeholder_text(textarea, text);
    }
}

void UITextBox::setPasswordMode(bool enabled) {
    if (textarea) {
        lv_textarea_set_password_mode(textarea, enabled);
    }
}

void UITextBox::setText(const char* text) {
    if (textarea) {
        lv_textarea_set_text(textarea, text);
    }
}

String UITextBox::getText() {
    if (textarea) {
        return String(lv_textarea_get_text(textarea));
    }
    return "";
}

void UITextBox::clear() {
    if (textarea) {
        lv_textarea_set_text(textarea, "");
    }
}

void UITextBox::focus() {
    if (textarea) {
        lv_obj_add_state(textarea, LV_STATE_FOCUSED);
    }
}

void UITextBox::onClicked(std::function<void()> callback) {
    clickedCallback = callback;
}

void UITextBox::onSubmit(std::function<void(String)> callback) {
    submitCallback = callback;
}

void UITextBox::setCursorColor(uint8_t r, uint8_t g, uint8_t b) {
    if (textarea) {
        uint32_t color = (r << 16) | (g << 8) | b;
        lv_obj_set_style_border_color(textarea, lv_color_hex(color), LV_PART_CURSOR);
        lv_obj_set_style_bg_color(textarea, lv_color_hex(color), LV_PART_CURSOR);
    }
}

UIKeyboard::UIKeyboard() : keyboard(nullptr), linkedTextBox(nullptr), visible(false) {}

UIKeyboard::~UIKeyboard() {
    // Clear the textbox's back pointer, so it does not call into this keyboard after it is gone
    if (linkedTextBox) {
        linkedTextBox->linkedKeyboard = nullptr;
        linkedTextBox = nullptr;
    }
    if (keyboard && lv_obj_is_valid(keyboard)) {
        lv_obj_delete(keyboard);
    }
}

void UIKeyboard::spawn(lv_obj_t* parent, int x, int y, int w, int h) {
    if (!parent) parent = lv_screen_active();
    
    if (keyboard && lv_obj_is_valid(keyboard)) {
        lv_obj_delete(keyboard);
        keyboard = nullptr;
    }
    
    keyboard = lv_keyboard_create(parent);
    
    lv_obj_set_size(keyboard, w, h);
    lv_obj_set_pos(keyboard, x, y);
    lv_obj_move_foreground(keyboard);
    
    lv_obj_set_style_pad_all(keyboard, 0, 0);
    lv_obj_set_style_pad_gap(keyboard, 2, 0);
    
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xffffff), LV_PART_ITEMS);
    
    lv_obj_set_style_pad_all(keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_gap(keyboard, 2, LV_PART_ITEMS);
    
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
    
    lv_obj_set_style_min_height(keyboard, 40, LV_PART_ITEMS);
    lv_obj_set_style_min_width(keyboard, 40, LV_PART_ITEMS);
    
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x2196F3), LV_PART_ITEMS | LV_STATE_PRESSED);
    
    lv_obj_add_event_cb(keyboard, event_handler, LV_EVENT_ALL, this);

    // spawn() creates a new keyboard object, so re-apply the textbox link
    // (this also covers a linkTo() call made before the first spawn()).
    if (linkedTextBox && linkedTextBox->getObject()) {
        lv_keyboard_set_textarea(keyboard, linkedTextBox->getObject());
    }

    visible = true;
}

void UIKeyboard::linkTo(UITextBox* textbox) {
    // Detach the previous textbox, and take this textbox away from any other keyboard
    if (linkedTextBox && linkedTextBox != textbox) {
        linkedTextBox->linkedKeyboard = nullptr;
    }
    if (textbox && textbox->linkedKeyboard && textbox->linkedKeyboard != this) {
        textbox->linkedKeyboard->unlinkTextBox(textbox);
    }

    linkedTextBox = textbox;
    if (textbox) {
        textbox->linkedKeyboard = this;
    }

    if (keyboard && textbox && textbox->getObject()) {
        lv_keyboard_set_textarea(keyboard, textbox->getObject());
        lv_obj_add_state(textbox->getObject(), LV_STATE_FOCUSED);
    } else if (keyboard && !textbox) {
        lv_keyboard_set_textarea(keyboard, nullptr);
    }
}

void UIKeyboard::unlinkTextBox(UITextBox* textbox) {
    if (linkedTextBox != textbox) return;
    linkedTextBox = nullptr;
    if (keyboard && lv_obj_is_valid(keyboard)) {
        lv_keyboard_set_textarea(keyboard, nullptr);
    }
}

void UIKeyboard::event_handler(lv_event_t* e) {
    UIKeyboard* instance = (UIKeyboard*)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_READY) {
        // Do not log the text: it may come from a password-mode textbox.
        Serial.println("Keyboard: Enter pressed");
        if (instance && instance->linkedTextBox) {
            String text = instance->linkedTextBox->getText();
            if (instance->enterCallback) {
                instance->enterCallback(text);
            }
            // The callback may have destroyed the textbox, which clears the link, so read the link again
            if (instance->linkedTextBox) {
                instance->linkedTextBox->submitText();
            }
        }
        if (instance) {
            instance->hide();
        }
    }
    else if (code == LV_EVENT_CANCEL) {
        Serial.println("Keyboard: Cancel pressed");
        if (instance && instance->cancelCallback) {
            instance->cancelCallback();
        }
        if (instance) {
            instance->hide();
        }
    }
}

void UIKeyboard::hide() {
    if (keyboard) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        visible = false;
    }
}

bool UIKeyboard::isVisible() {
    return visible && keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void UIKeyboard::onEnter(std::function<void(String)> callback) {
    enterCallback = callback;
}

void UIKeyboard::onCancel(std::function<void()> callback) {
    cancelCallback = callback;
}

void UIKeyboard::setButtonSize(int min_width, int min_height) {
    if (keyboard) {
        lv_obj_set_style_min_height(keyboard, min_height, LV_PART_ITEMS);
        lv_obj_set_style_min_width(keyboard, min_width, LV_PART_ITEMS);
    }
}
