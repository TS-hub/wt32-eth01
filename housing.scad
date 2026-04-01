/* ================================================================
   iperf Test Plug — 3D-printed housing
   Waveshare ESP32-S3-ETH (23 × 72 mm)
   SH1106 1.3" OLED (35.6 × 33.6 mm PCB)

   Two parts: body + lid — print separately, flat face down.
   No supports required.

   Assembly:
     1. Place body with open face up.
     2. Lower board in; connectors align with cutouts on short ends.
     3. Place display PCB on lid posts (screen toward window).
     4. Fasten display with 4× M3 × 6 self-tapping screws from inside.
     5. Press lid onto body.
   ================================================================ */

/* ── Board ────────────────────────────────────────────────────── */
board_w  = 23.0;    // PCB width
board_l  = 72.0;    // PCB length
board_t  =  1.6;    // PCB thickness
standoff =  3.5;    // post height below PCB — must exceed pin-header tail length

/* ── Connector openings (z = 0 is PCB bottom face) ───────────── */
// RJ45 — front end (y = 0 side of housing)
rj45_w   = 16.2;
rj45_h   = 14.0;
rj45_z   =  0.0;

// USB-C — back end (y = outer_l side of housing)
usbc_w   =  9.5;
usbc_h   =  3.5;
usbc_z   =  0.5;   // USB-C port sits 0.5 mm above PCB bottom

/* ── SH1106 1.3" display module ───────────────────────────────── */
dm_w     = 33.6;   // module PCB width  (across housing width, X axis)
dm_l     = 35.6;   // module PCB length (along housing length, Y axis)
dm_t     =  1.5;   // PCB thickness
dm_hole  =  3.0;   // mounting hole diameter
dm_hm    =  2.5;   // hole centre distance from each PCB edge

// Visible screen area (centred on the PCB, adjust if needed)
scr_w    = 29.4;
scr_h    = 14.7;

/* ── Shell geometry ───────────────────────────────────────────── */
wall     = 2.0;
floor_t  = 1.5;
corner_r = 2.0;    // outer corner radius

/* ── Lid ──────────────────────────────────────────────────────── */
lid_top  = 3.0;    // top plate thickness
post_h   = 5.0;    // display mounting post height (hangs into body cavity)
rim_ins  = 5.0;    // press-fit rim insertion depth
rim_t    = 1.5;    // rim wall thickness
rim_lc   = 0.25;   // press-fit clearance each side

/* ── Derived (do not edit) ────────────────────────────────────── */
inner_w  = dm_w + 2.4;             // 36.0 — 1.2 mm clearance each side for display
inner_l  = board_l + 1.0;          // 73.0
pcb_bot  = floor_t + standoff;     // z of PCB bottom from housing floor: 5.0
inner_h  = pcb_bot - floor_t + board_t + rj45_h + 0.5;  // clears RJ45: 19.6 → 20
inner_h  = ceil(inner_h / 1) * 1; // round up

outer_w  = inner_w + 2*wall;       // 40.0
outer_l  = inner_l + 2*wall;       // 77.0
body_h   = floor_t + inner_h;      // 21.5

// Board centred in X
bx       = (inner_w - board_w) / 2;  // 6.5

// Display centred in X, offset 8 mm from RJ45 end in Y
dm_xo    = (inner_w - dm_w) / 2;     // 1.2
dm_yo    = 8.0;

// Screen window position relative to inner cavity origin
scr_xo   = dm_xo + (dm_w - scr_w) / 2;
scr_yo   = dm_yo + (dm_l - scr_h) / 2;

echo(str("Body outer: ",  outer_w, " × ", outer_l, " × ", body_h,           " mm"));
echo(str("Lid height: ",  lid_top + post_h,                                  " mm"));
echo(str("Total height: ", outer_w, " × ", outer_l, " × ", body_h + lid_top," mm"));

/* ── Utilities ────────────────────────────────────────────────── */

// Rounded rectangle (2D)
module rrect(w, l, r) {
    offset(r=r, $fn=32) square([w - 2*r, l - 2*r]);
}

/* ================================================================
   BODY
   ================================================================ */
module body() {
    difference() {
        // Outer shell
        linear_extrude(body_h)
            rrect(outer_w, outer_l, corner_r);

        // Inner cavity — open top
        translate([wall, wall, floor_t])
            cube([inner_w, inner_l, inner_h + 1]);

        // RJ45 opening — front face (y = 0)
        translate([(outer_w - rj45_w)/2, -1, pcb_bot + rj45_z])
            cube([rj45_w, wall + 2, rj45_h]);

        // USB-C opening — back face (y = outer_l)
        translate([(outer_w - usbc_w)/2, outer_l - wall - 1, pcb_bot + usbc_z])
            cube([usbc_w, wall + 2, usbc_h]);
    }

    // ── Board support posts (4 corners, inset 3 mm from board edge)
    post_r = 1.5;
    inset  = 3.0;
    for (px = [bx + inset,        bx + board_w - inset])
    for (py = [wall + inset,      wall + inner_l - inset])
        translate([px, py, floor_t])
            cylinder(h=standoff, r=post_r, $fn=24);

    // ── Board lateral guides — short rails that locate the board in X
    //    (sit flush with the board-side inner wall, height spans standoff + PCB)
    guide_h = standoff + board_t + 1.0;
    guide_w = bx - 0.3;              // fills gap between inner wall and board edge
    guide_l = 12.0;
    for (side = [0, 1])
        mirror([side, 0, 0])
            translate([wall, wall + (inner_l - guide_l)/2, floor_t])
                cube([guide_w, guide_l, guide_h]);
}

/* ================================================================
   LID
   Modelled with z=0 at outer (top) face, positive Z downward into cavity.
   Flip 180° around X for printing (flat face on bed).
   ================================================================ */
module lid() {
    // Hole centres relative to display PCB origin
    holes = [
        [dm_hm,        dm_hm       ],
        [dm_w - dm_hm, dm_hm       ],
        [dm_hm,        dm_l - dm_hm],
        [dm_w - dm_hm, dm_l - dm_hm]
    ];

    difference() {
        union() {
            // Top plate
            linear_extrude(lid_top)
                rrect(outer_w, outer_l, corner_r);

            // Press-fit rim (extends in +Z = downward into body in use)
            translate([wall + rim_lc, wall + rim_lc, lid_top])
                difference() {
                    cube([inner_w - 2*rim_lc, inner_l - 2*rim_lc, rim_ins]);
                    translate([rim_t, rim_t, -0.1])
                        cube([inner_w - 2*rim_lc - 2*rim_t,
                              inner_l - 2*rim_lc - 2*rim_t,
                              rim_ins + 0.2]);
                }

            // Display mounting posts (extend in +Z = down into cavity)
            for (h = holes)
                translate([wall + dm_xo + h[0], wall + dm_yo + h[1], lid_top])
                    cylinder(h=post_h, r=dm_hole/2 + 1.2, $fn=24);
        }

        // Screen window — through full top plate
        translate([wall + scr_xo, wall + scr_yo, -1])
            cube([scr_w, scr_h, lid_top + 2]);

        // Screw holes through posts (M3 pilot: 2.5 mm diameter)
        for (h = holes)
            translate([wall + dm_xo + h[0], wall + dm_yo + h[1], lid_top - 0.1])
                cylinder(h=post_h + 0.2, r=1.25, $fn=20);

        // Countersink / access opening from cavity side (so screws can be driven)
        // — screw heads come from inside the cavity, posts are the nut side
        // Nothing to cut here: screws self-tap into posts from inside.

        // Label (embossed on outer top face, readable when looking at display)
        translate([outer_w/2, outer_l - wall - 6, -0.4])
            rotate([0, 0, 180])
            linear_extrude(0.5)
                text("iperf test plug", size=3.5, halign="center", valign="center",
                     font="Liberation Sans:style=Bold");
    }
}

/* ================================================================
   RENDER
   ================================================================ */
// Body — print as-is (floor down)
body();

// Lid — shown beside body, flipped for printing (outer top face on bed)
translate([outer_w + 10, 0, lid_top + post_h])
    rotate([180, 0, 0])
        lid();
