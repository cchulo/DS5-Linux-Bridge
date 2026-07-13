// ============================================================
// Raspberry Pi Pico -> 2.5" drive mount, Fractal panel version
// (DS5Dongle) - bracket screws to the INSIDE of the case's 1mm
// inner sheet using the 2.5" SSD bottom-mount pattern; the Pico
// mounts on the OUTWARD face, poking through the panel opening,
// with its PCB top flush with the outer relief surface.
//
// Height stack from the bracket's mounting face:
//   1.0 inner sheet + 2.0 relief depth = 3.0 to outer surface
//   standoff protrusion = 3.0 - 1.0 (PCB) = 2.0mm
//   -> PCB top lands exactly on the relief plane
//
// Pico: M2 screws into M2 x 4 x 3.2 inserts in the standoff tips.
// Case: M3 screws into M3 x 4 x 5 inserts in the mounting face.
//
// Print flat face down (standoffs up - also the insert side up).
// ============================================================

/* ---------- Parameters ---------- */

// 2.5" drive footprint (SFF-8201 bottom pattern)
drive_len       = 100.0;
drive_wid       = 69.85;
plate_t         = 5.0;     // thick enough for the M3 inserts

// Case geometry
sheet_t         = 1.0;     // inner sheet thickness
relief_depth    = 2.0;     // recess depth from sheet to outer surface
grommet_offset  = 0.0;     // extra gap if rubber grommets hold the
                           // bracket off the sheet - measure & set

// Bottom mounting holes: M3 x 4 x 5 heat-set inserts, pressed
// into the mounting face; case screws come through the sheet.
bot_hole_x      = [14.0, 90.6];
bot_hole_inset  = 4.065;               // 61.72mm apart across width
m3_insert_hole  = 4.7;                 // for OD 5.0 insert (try 4.6 if loose)
m3_insert_len   = 4.0;

// Raspberry Pi Pico
pico_len        = 51.0;
pico_wid        = 21.0;
pico_pcb_t      = 1.0;
pico_hole_dx    = 47.0;
pico_hole_dy    = 11.4;
pico_x_offset   = 0.0;                 // shift from center along length
pico_y_offset   = 0.0;                 // shift from center across width

// I-shape cutouts: open sides between the end bands so wires
// and dupont connectors tuck behind the bracket
band_w          = 22.0;    // solid band at each end (covers M3 inserts)
spine_w         = 25.0;    // center spine width (covers Pico + standoffs)
cut_r           = 3.0;     // corner radius of the cutouts

// M2 x 4 x 3.2 heat-set inserts
insert_hole_d   = 3.0;                 // for OD 3.2 insert (try 2.9 if loose)
insert_len      = 4.0;
standoff_d      = 6.0;                 // wall around the insert

$fn = 48;

/* ---------- Derived ---------- */
protrusion = sheet_t + relief_depth + grommet_offset - pico_pcb_t;  // = 2.0
mid_y      = drive_wid / 2 + pico_y_offset;
pico_x1    = (drive_len - pico_len)/2 + pico_x_offset;   // centered
hole_xs    = [pico_x1 + (pico_len - pico_hole_dx)/2,
              pico_x1 + (pico_len + pico_hole_dx)/2];
hole_ys    = [mid_y - pico_hole_dy/2, mid_y + pico_hole_dy/2];
bot_hole_y = [bot_hole_inset, drive_wid - bot_hole_inset];

// Rounded-corner cutout helper
module rounded_slot(x0, y0, x1, y1, r, h) {
    hull()
        for (cx = [x0 + r, x1 - r], cy = [y0 + r, y1 - r])
            translate([cx, cy, -0.01]) cylinder(r = r, h = h + 0.02);
}

/* ---------- Main ----------
   z=0..plate_t is the plate; z=plate_t is the MOUNTING face
   (contacts the inner sheet); standoffs rise past it.        */
difference() {
    union() {
        cube([drive_len, drive_wid, plate_t]);

        // Standoffs protruding past the mounting face, through
        // the panel opening; Pico screws down onto their tips.
        for (x = hole_xs, y = hole_ys)
            translate([x, y, plate_t])
                cylinder(d = standoff_d, h = protrusion);
    }

    // I-shape side cutouts (extend past the edges -> open sides)
    for (sy = [-1, 1]) {
        y_in  = mid_y + sy * spine_w/2;               // spine edge
        y_out = sy > 0 ? drive_wid + 10 : -10;        // beyond plate edge
        rounded_slot(band_w, min(y_in, y_out),
                     drive_len - band_w, max(y_in, y_out),
                     cut_r, plate_t + 10);
    }

    // M3 insert bores (blind from the mounting face, 0.6mm floor)
    for (x = bot_hole_x, y = bot_hole_y)
        translate([x, y, plate_t - (m3_insert_len + 0.4)]) {
            cylinder(d = m3_insert_hole, h = m3_insert_len + 0.41);
            translate([0, 0, m3_insert_len - 0.09])
                cylinder(d1 = m3_insert_hole, d2 = m3_insert_hole + 1.0, h = 0.5); // lead-in
        }

    // Heat-set insert holes in the standoff tips (blind, 0.6mm floor)
    for (x = hole_xs, y = hole_ys)
        translate([x, y, plate_t + protrusion - (insert_len + 0.4)])
            cylinder(d = insert_hole_d, h = insert_len + 0.41);
}
