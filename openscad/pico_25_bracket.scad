// ============================================================
// Raspberry Pi Pico -> 2.5" drive mount, "window" version, v3
// (DS5Dongle, Fractal Ridge front panel)
//
// Full rectangular plate. Mounts to the case inner sheet via
// M3 x 4 x 5 inserts (SFF-8201 bottom pattern). Pico mounts
// UPSIDE-DOWN against the interior face: component side toward
// the plate, exposed through the window; USB plug + cable exit
// through an open-ended channel (roofed by the case sheet).
//
// FOUR M2 x 4 x 3.2 insert points - no posts. The board seats
// on four low 2mm pads that are solid extensions of the plate;
// each bore runs up through pad + plate (6.6mm of material
// around a 4.4mm bore). Pin stubs on the component face hang
// in the 2mm air gap; the jack pokes into the channel neck.
// All four screws: M2 x 6.
//
// NOTE: at the USB end the bore wall toward the channel neck is
// ~0.2mm - it prints as a fused perimeter and may bulge a hair
// into the neck when heat-setting; harmless, jack still clears.
// NOTE: the LED sits under solid plate (not visible); BOOTSEL
// is in the window.
//
// Print mounting face down. No supports needed.
// ============================================================

/* ---------- Parameters ---------- */

drive_len       = 100.0;
drive_wid       = 69.85;
plate_t         = 5.0;
cut_r           = 3.0;

// M3 x 4 x 5 inserts (case screws come through the sheet)
bot_hole_x      = [14.0, 90.6];
bot_hole_inset  = 4.065;
m3_insert_hole  = 4.7;
m3_insert_len   = 4.0;

// Raspberry Pi Pico, upside-down on the interior face
pico_len        = 51.0;
pico_hole_dx    = 47.0;
pico_hole_dy    = 11.4;
pico_x_offset   = 0.0;
pico_y_offset   = 0.0;
usb_end_high_x  = true;    // jack toward the x=100 end

// Board seat pads (solid part of the plate)
pad_h           = 2.0;     // air gap under the board (> pin stubs)
pad_len         = 6.0;     // pad size along x
pad_in          = 4.0;     // pad inner face (= channel neck wall)
pad_out         = 8.0;     // pad outer face (clears pin rows at 8.25)

// M2 x 4 x 3.2 inserts
insert_hole_d   = 3.0;
insert_len      = 4.0;

// Window + USB channel
window_hw       = 9.5;
neck_hw         = 4.0;     // channel neck over the jack (plug nose ~6.9mm)
chan_hw         = 6.0;     // main channel (plug body ~11mm)

$fn = 48;

/* ---------- Derived ---------- */
mid_y      = drive_wid / 2 + pico_y_offset;
bx0        = (drive_len - pico_len)/2 + pico_x_offset;
bx1        = bx0 + pico_len;
usb_x      = usb_end_high_x ? bx1 : bx0;
sgn        = usb_end_high_x ? 1 : -1;
ant_hole_x = usb_x - sgn * (2 + pico_hole_dx);   // 2mm from each board end
usb_hole_x = usb_x - sgn * 2;
hole_ys    = [mid_y - pico_hole_dy/2, mid_y + pico_hole_dy/2];
bot_hole_y = [bot_hole_inset, drive_wid - bot_hole_inset];

win_a   = ant_hole_x + sgn * 2.0;                // past antenna pads
win_b   = usb_hole_x - sgn * 3.6;                // before USB pads
wx0     = min(win_a, win_b);  wx1 = max(win_a, win_b);
nk_a    = win_b;  nk_b = usb_x + sgn * 1.6;      // neck spans the jack
ch_a    = nk_b - sgn * 0.5;                      // main channel to the end
ch_b    = usb_end_high_x ? drive_len + 10 : -10;

module rounded_slot(x0, y0, x1, y1, r, h) {
    hull()
        for (cx = [x0 + r, x1 - r], cy = [y0 + r, y1 - r])
            translate([cx, cy, -0.01]) cylinder(r = r, h = h + 0.02);
}

/* ---------- Main ----------
   z 0..plate_t = plate; z = plate_t = mounting face (on sheet);
   interior side is z < 0. Board component face seats at -pad_h. */
difference() {
    union() {
        cube([drive_len, drive_wid, plate_t]);

        // Four board seat pads, solid with the plate above them
        for (hx = [ant_hole_x, usb_hole_x], sy = [-1, 1])
            translate([hx - pad_len/2,
                       mid_y + (sy > 0 ? pad_in : -pad_out),
                       -pad_h])
                cube([pad_len, pad_out - pad_in, pad_h + 0.01]);
    }

    // Front window exposing the Pico's component face
    rounded_slot(wx0, mid_y - window_hw, wx1, mid_y + window_hw,
                 cut_r, plate_t);

    // Channel neck over the jack (between the USB pads)
    rounded_slot(min(nk_a, nk_b) - 2, mid_y - neck_hw,
                 max(nk_a, nk_b) + 2, mid_y + neck_hw,
                 2.0, plate_t);

    // Main cable channel, open out the end of the bracket
    rounded_slot(min(ch_a, ch_b), mid_y - chan_hw,
                 max(ch_a, ch_b), mid_y + chan_hw,
                 cut_r, plate_t);

    // M3 insert bores (blind from the mounting face)
    for (x = bot_hole_x, y = bot_hole_y)
        translate([x, y, plate_t - (m3_insert_len + 0.4)]) {
            cylinder(d = m3_insert_hole, h = m3_insert_len + 0.41);
            translate([0, 0, m3_insert_len - 0.09])
                cylinder(d1 = m3_insert_hole, d2 = m3_insert_hole + 1.0, h = 0.5);
        }

    // M2 insert bores through the pads into the plate
    for (hx = [ant_hole_x, usb_hole_x], y = hole_ys)
        translate([hx, y, -pad_h - 0.01])
            cylinder(d = insert_hole_d, h = insert_len + 0.41);
}
