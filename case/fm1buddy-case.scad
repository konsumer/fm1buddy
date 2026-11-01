// The fm1buddy case for the CYD-S3 (Freenove FNK0104B, ESP32-S3 2.8"): snap-fit, no screws, printed without supports
// (base: open side up; lid: top side down).
//
//   battery  room for a flat LiPo under the board (switch_hole: and a hole for a slide switch in its lead)
//   part     base, lid, or preview (the case open, with a stand-in for the board)
//
// openscad -D 'part="lid"' -o lid.stl fm1buddy-case.scad (tools/cases.sh makes them all)
//
// The board's size is from LCDwiki's ES3C28P outline drawing (the same board). x from its USB-C end, y across it, z up
// from the floor.

/* [What] */
battery = false;
part = "preview";  // [preview, base, lid]
section = false;   // preview: cut in half, to see the inside

/* [Fit] */
wall = 2.0;
floor_t = 1.6;
lid_t = 1.6;
gap = 0.6;    // around the boards
fit = 0.25;   // between the lid's lip and the walls
lip_h = 4;

/* [Battery] */
bat = [68, 52, 11];  // room for a 10 mm 3000 mAh cell: 105060, 104060, 103665 (MakerFocus's), or 7.5 x 50 x 60; 503035 (500 mAh): [36, 31, 5.2]
switch_hole = [0, 0];  // none (the board sleeps by itself); [9, 4.2]: a mini slide switch (SS12D00) in its lead

$fn = 48;
LID_COLOR = "#ff8a3d";   // the installer page's orange
BASE_COLOR = "#36b37e";
PCB = 1.57;
eps = 0.01;

// ---------------------------------------------------------------------------------------------- board ---
// the CYD-S3, face up: x from its USB-C end, y from the long edge with the mic (the SD card, BAT and BOOT are on the
// other). USB-C, BOOT, RESET, the SD socket and the RGB LED are on its back
S3 = [86, 50];
S3_HOLES = [[4, 4], [82, 4], [4, 46], [82, 46]];
S3_FRONT = 4.3;                                              // the display and touch panel, above its board
S3_BACK = 4.7;                                               // its tallest parts underneath
S3_WIN = [[16.0, 2.6], [76.0, 47.4]];                        // the window: visible area 58.05 x 43.6 at x 17 .. 75
S3_USB = [25, 3.2];                                          // its USB-C: y of the middle, height under the board
S3_BTN = [[3.26, 36.57], [3.26, 13.44]];                     // BOOT, RESET
S3_SD = 50.97;                                               // the card goes in through the y = 50 edge, here
S3_LED = [44.6, 17.6];

// ------------------------------------------------------------------------------------------- shapes ---
module rbox(p0, p1, r = 1.5) {
  s = p1 - p0;
  translate([p0.x + r, p0.y + r, p0.z]) linear_extrude(s.z) offset(r) square([s.x - 2 * r, s.y - 2 * r]);
}

// a hole through a wall, centered on c (the wall's plane is given by axis: 0 = x wall, 1 = y wall)
module wall_hole(axis, c, w, h, r = 1) {
  t = wall + gap + 2;
  if (axis == 0) translate([c.x - t / 2, c.y - w / 2, c.z - h / 2]) rotate([90, 0, 90]) linear_extrude(t) offset(r) offset(-r) square([w, h]);
  else translate([c.x - w / 2, c.y + t / 2, c.z - h / 2]) rotate([90, 0, 0]) linear_extrude(t) offset(r) offset(-r) square([w, h]);
}

// --------------------------------------------------------------------------------------- the box ---
// cavity [x0, y0] .. [x1, y1], height h (from the floor's top); a snap groove along both long sides
module tray(c0, c1, h) {
  difference() {
    rbox([c0.x - wall, c0.y - wall, -floor_t], [c1.x + wall, c1.y + wall, h], 3);
    rbox([c0.x, c0.y, 0], [c1.x, c1.y, h + 1], 1.5);
    for (y = [c0.y, c1.y]) translate([(c0.x + c1.x) / 2, y, h - lip_h / 2]) rotate([0, 90, 0]) cylinder(r = 0.7, h = (c1.x - c0.x) * 0.5, center = true);
  }
}

// the lid: a plate on the walls, a lip inside them with bumps that click into the grooves
module lid_plate(c0, c1) {
  rbox([c0.x - wall, c0.y - wall, 0], [c1.x + wall, c1.y + wall, lid_t], 3);
  difference() {
    rbox([c0.x + fit, c0.y + fit, -lip_h], [c1.x - fit, c1.y - fit, 0], 1.2);
    rbox([c0.x + fit + 1.2, c0.y + fit + 1.2, -lip_h - 1], [c1.x - fit - 1.2, c1.y - fit - 1.2, 1], 0.5);
  }
  for (y = [c0.y + fit, c1.y - fit]) translate([(c0.x + c1.x) / 2, y, -lip_h / 2]) rotate([0, 90, 0]) cylinder(r = 0.55, h = (c1.x - c0.x) * 0.45, center = true);
}

// posts with pegs, for a board's holes
module pegs(holes, h, d_post, d_peg, peg_h) {
  for (p = holes) translate([p.x, p.y, 0]) {
    cylinder(d = d_post, h = h);
    cylinder(d = d_peg, h = h + peg_h);
  }
}

// --------------------------------------------------------------------------------------------- CYD-S3 ---
// the board face up on posts (high enough for a battery under it), its USB-C through the x = 0 wall for the OTG
// adapter. The box is wider than the board across, so the lid's lip clears the display glass (as wide as the
// board); posts in the lid hold the board down at its holes. Paperclip holes in the floor reach BOOT and RESET
s_bz = (battery ? bat.z + 0.7 : 0) + S3_BACK + 0.8;           // the board's underside
s_inner_h = s_bz + PCB + S3_FRONT + 0.2;
sc0 = [-gap, -gap - 1.5];
sc1 = [S3.x + gap, S3.y + gap + 1.5];
// the battery's corner: in the middle, clear of the posts (x 7 .. 79) and the paperclip holes, wall to wall across
s_bat = [S3.x / 2 - bat.x / 2, S3.y / 2 - bat.y / 2];
assert(!battery || (bat.x <= 72 && bat.y <= 53), "the battery is too big: 72 x 53 at most");

module s3_base() {
  usb_z = s_bz - S3_USB.y / 2;
  difference() {
    union() {
      tray(sc0, sc1, s_inner_h);
      pegs(S3_HOLES, s_bz, 5.4, 2.9, PCB + 0.5);
    }
    // the OTG adapter's plug (its body is bigger than the socket)
    wall_hole(0, [sc0.x - wall / 2, S3_USB.x, usb_z], 13, 7.5, 2);
    // the SD card, and a notch to push it in with a fingernail
    wall_hole(1, [S3_SD, sc1.y + wall / 2, s_bz - 1.0], 16, 4, 0.8);
    translate([S3_SD, sc1.y - 1, s_bz - 1.0]) rotate([-90, 0, 0]) cylinder(d = 9, h = wall + 2);
    // BOOT and RESET, for a paperclip from underneath
    for (b = S3_BTN) translate([b.x + 0.4, b.y, -floor_t - 1]) cylinder(d = 2.4, h = floor_t + 2);
    // thin under the RGB LED, so it shows through (a battery covers it)
    if (!battery) translate([S3_LED.x - 4, S3_LED.y - 4, -floor_t + 0.6]) cube([8, 8, floor_t]);
    if (battery && switch_hole.x > 0) wall_hole(1, [18, sc1.y + wall / 2, bat.z / 2 + 0.5], switch_hole.x, switch_hole.y, 0.5);
  }
}

module s3_lid() {
  down = s_inner_h - s_bz - PCB;  // from the lid's underside to the board
  difference() {
    union() {
      lid_plate(sc0, sc1);
      for (p = S3_HOLES) translate([p.x, p.y, -down]) cylinder(d = 4.6, h = down + eps);
    }
    translate([S3_WIN[0].x, S3_WIN[0].y, -lip_h - 1]) linear_extrude(lip_h + lid_t + 2) offset(1) offset(-1) square(S3_WIN[1] - S3_WIN[0]);
  }
}

// ---------------------------------------------------------------------------------------- previews ---
module stand_in() {
  translate([0, 0, s_bz]) color("#222") cube([S3.x, S3.y, PCB]);
  translate([8.4, 0, s_bz + PCB]) color("#111") cube([69.2, S3.y, S3_FRONT - 0.2]);
  translate([-0.3, S3_USB.x - 4.5, s_bz - S3_USB.y]) color("#aaa") cube([7.3, 9, S3_USB.y]);
  translate([S3_SD - 7, S3.y - 15, s_bz - 1.9]) color("#bbb") cube([14, 15, 1.9]);
  if (part != "view_board") battery_in();
}

module battery_in() {
  if (battery) translate([s_bat.x, s_bat.y, 0.3]) color("silver") cube(bat);
}

// --------------------------------------------------------------------------------------------- make ---
module cut() {
  if (section) intersection() {
    children();
    translate([-200, 25, -200]) cube(400);
  }
  else children();
}

cut() {
  if (part == "base") s3_base();
  if (part == "lid") translate([0, 0, lid_t]) mirror([0, 0, 1]) s3_lid();
  if (part == "preview") {
    color(BASE_COLOR) s3_base();
    stand_in();
    translate([0, 0, s_inner_h + 25]) color(LID_COLOR) s3_lid();
  }
  // the preview's pieces one at a time (an STL has no colors), for the installer page's 3D view (casemaker.js)
  if (part == "view_lid") translate([0, 0, s_inner_h + 25]) s3_lid();
  if (part == "view_board") stand_in();
  if (part == "view_battery") battery_in();
}
