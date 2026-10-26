#ifndef SIMPLEUI_H
#define SIMPLEUI_H

#include <Arduino.h>
#include <lvgl.h>

// This library targets LVGL 9.x (see README). Fail early on other versions.
#if LVGL_VERSION_MAJOR != 9
#error "ESP_DISPLAY_TOUCH requires LVGL 9.x"
#endif
#include <functional>

// ==================== Global Functions ====================
void SimpleUI_init();
void SimpleUI_update();

// ==================== Screen Class ====================
class UIScreen {
private:
    lv_obj_t* screen;

public:
    UIScreen();
    ~UIScreen();
    void load();
    void setBackgroundColor(uint8_t r, uint8_t g, uint8_t b);
    lv_obj_t* getObject() { return screen; }
};

// ==================== Label Class ====================
class UILabel {
private:
    lv_obj_t* label;

public:
    UILabel();
    ~UILabel();
    void create(lv_obj_t* parent = nullptr, int x = 0, int y = 0, int w = LV_SIZE_CONTENT);
    void setText(const char* text);
    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setFontSize(uint8_t size);
    void hide();
    void show();
    lv_obj_t* getObject() { return label; }
};

// ==================== Button Class ====================
class UIButton {
private:
    lv_obj_t* btn;
    lv_obj_t* label;
    std::function<void()> clickCallback;

    static void event_handler(lv_event_t* e);

public:
    UIButton();
    ~UIButton();
    void create(lv_obj_t* parent, const char* text, int x, int y, int w, int h);
    void onClick(std::function<void()> callback);
    void setBackgroundColor(uint8_t r, uint8_t g, uint8_t b);
    void setText(const char* text);
    void hide();
    void show();
    void enable();
    void disable();
    lv_obj_t* getObject() { return btn; }
};

// ==================== TextBox Class ====================
class UIKeyboard;

class UITextBox {
private:
    lv_obj_t* textarea;
    UIKeyboard* linkedKeyboard; // keyboard currently linked to this box, or nullptr
    std::function<void()> clickedCallback;
    std::function<void(String)> submitCallback;

    static void event_handler(lv_event_t* e);
    friend class UIKeyboard;

public:
    UITextBox();
    ~UITextBox();
    void create(lv_obj_t* parent, int x, int y, int w, int h);
    void setPlaceholder(const char* text);
    void setPasswordMode(bool enabled);
    void setText(const char* text);
    String getText();
    void clear();
    void focus();
    void onClicked(std::function<void()> callback);
    void onSubmit(std::function<void(String)> callback);
    void setCursorColor(uint8_t r, uint8_t g, uint8_t b);
    void submitText();
    lv_obj_t* getObject() { return textarea; }
};

// ==================== Keyboard Class ====================
class UIKeyboard {
private:
    lv_obj_t* keyboard;
    UITextBox* linkedTextBox;
    bool visible;
    std::function<void(String)> enterCallback;
    std::function<void()> cancelCallback;

    static void event_handler(lv_event_t* e);
    void unlinkTextBox(UITextBox* textbox);
    friend class UITextBox;

public:
    UIKeyboard();
    ~UIKeyboard();
    void spawn(lv_obj_t* parent, int x, int y, int w, int h);
    void linkTo(UITextBox* textbox);
    void hide();
    bool isVisible();
    void onEnter(std::function<void(String)> callback);
    void onCancel(std::function<void()> callback);
    void setButtonSize(int min_width, int min_height);
    lv_obj_t* getObject() { return keyboard; }
};

#endif // SIMPLEUI_H
