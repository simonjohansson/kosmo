/* SPDX-License-Identifier: GPL-3.0-only */
/* The placeholder UI: one screen that says KOSMO (drawn once, at power-on) and the buttons that matter: OCT- and OCT+
 * shift the keys by an octave (both together: back to the middle). The real UI comes later. */
static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 100, 240, &AF_L, "KOSMO", T_THEME, 1);
}

static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0);
    uint32_t dn = 1u << panel.btn[B_OCTDN], up = 1u << panel.btn[B_OCTUP];
    if (!(pressed & (dn | up)))
        return;
    if ((fm1_in.buttons & (dn | up)) == (dn | up))
        song.octave = 0;
    else if ((pressed & dn) && song.octave > -3)
        song.octave--;
    else if ((pressed & up) && song.octave < 3)
        song.octave++;
}
