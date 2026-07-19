// ============================================================
// Raspberry Pi Pico -> 2.5" drive mount, v8 "clean"
// (DS5Dongle, Fractal Ridge front panel)
//
// Full rectangular plate, mounts to the case inner sheet via
// M3 x 4 x 5 inserts (SFF-8201 bottom pattern). The Pico
// mounts on the interior face, component side facing the case
// interior (right-angle pins, duponts, USB cable and BOOTSEL
// all in open case space), on four CIRCULAR standoffs (6mm
// dia, 5mm tall) with M2 x 4 x 3.2 inserts in the tips.
// Rounded notches on both side edges route RGB cables around
// the bracket.
//
// Screws: M2 x 6 (bores deepened so they don't bottom out).
// Print mounting face down. No supports needed.
// ============================================================

/* ---------- Parameters ---------- */

drive_len       = 100.0;
drive_wid       = 69.85;
plate_t         = 5.0;

// M3 x 4 x 5 inserts (case screws come through the sheet)
bot_hole_x      = [14.0, 90.6];
bot_hole_inset  = 4.065;
m3_insert_hole  = 4.7;
m3_insert_len   = 4.0;

// Raspberry Pi Pico (component side toward case interior)
pico_hole_dx    = 47.0;
pico_hole_dy    = 11.4;
pico_x_offset   = 0.0;
pico_y_offset   = 0.0;

// Circular standoffs
standoff_h      = 5.0;
standoff_d      = 6.0;     // 1.5mm walls around the bore

// M2 x 4 x 3.2 inserts
insert_hole_d   = 3.0;
insert_len      = 4.0;
m2_bore_depth   = 5.5;     // deep enough for M2 x 6 screws

// Rounded side notches for RGB cable routing
notch_r         = 10.0;
notch_x         = 50.0;
notch_low       = true;
notch_high      = true;

$fn = 64;

/* ---------- Derived ---------- */
mid_y      = drive_wid / 2 + pico_y_offset;
cx         = drive_len / 2 + pico_x_offset;
hole_xs    = [cx - pico_hole_dx/2, cx + pico_hole_dx/2];
hole_ys    = [mid_y - pico_hole_dy/2, mid_y + pico_hole_dy/2];
bot_hole_y = [bot_hole_inset, drive_wid - bot_hole_inset];

/* ---------- Main ----------
   z 0..plate_t = plate; z = plate_t = mounting face (on sheet);
   interior side is z < 0. Board back seats at -standoff_h. */
difference() {
    union() {
        cube([drive_len, drive_wid, plate_t]);

        // Four circular standoffs
        for (hx = hole_xs, y = hole_ys)
            translate([hx, y, -standoff_h])
                cylinder(d = standoff_d, h = standoff_h + 0.01);
    }

    // Rounded side notches for cable routing
    if (notch_low)
        translate([notch_x, 0, -standoff_h - 1])
            cylinder(r = notch_r, h = plate_t + standoff_h + 2);
    if (notch_high)
        translate([notch_x, drive_wid, -standoff_h - 1])
            cylinder(r = notch_r, h = plate_t + standoff_h + 2);

    // M3 insert bores (blind from the mounting face)
    for (x = bot_hole_x, y = bot_hole_y)
        translate([x, y, plate_t - (m3_insert_len + 0.4)]) {
            cylinder(d = m3_insert_hole, h = m3_insert_len + 0.41);
            translate([0, 0, m3_insert_len - 0.09])
                cylinder(d1 = m3_insert_hole, d2 = m3_insert_hole + 1.0, h = 0.5);
        }

    // M2 insert bores in the standoff tips (deepened for M2x6)
    for (hx = hole_xs, y = hole_ys)
        translate([hx, y, -standoff_h - 0.01])
            cylinder(d = insert_hole_d, h = m2_bore_depth + 0.01);
}
