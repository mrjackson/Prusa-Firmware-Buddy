//screen_qr_error.hpp
#pragma once
#include "gui.hpp"
#include "window_text.hpp"
#include <array>
#include "screen.hpp"
#include "screen_reset_error.hpp"
#include "window_header.hpp"
#include "status_footer.hpp"
#if HAS_LEDS
    #include "led_animations/animator.hpp"
#endif

struct ScreenErrorQR : public AddSuperWindow<screen_reset_error_data_t> {

    window_header_t header;
    StatusFooter footer;
    window_text_t err_title;
    window_text_t err_description;
    window_icon_t hand_icon;
    window_qr_t qr;
    window_text_t help_txt;
    window_text_t help_link;
    window_text_t qr_code_txt;
    window_text_t fw_version_txt;
    window_text_t signature_txt;
    window_text_t appendix_txt;
#if HAS_LEDS
    AnimatorLCD::AnimationGuard anim;
#endif
    window_t title_line;

public:
    ScreenErrorQR();

protected:
    virtual void windowEvent(EventLock /*has private ctor*/, window_t *sender, GUI_event_t event, void *param) override;
};
