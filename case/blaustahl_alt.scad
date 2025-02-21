/*
 * Blaustahl Alt Case
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * required hardware:
 *  - 1 x M2.5 x 12mm countersunk bolt
 *  - 1 x M2.5 nut
 *
 */
 
$fn = 30;

box_width = 19;
box_length = 37;
box_height = 12.5;
box_thickness = 2.0;

lid_thickness = 3;

cutout_width = 16.10;
cutout_length = 40;
cutout_height = 6.5;

cutout_usb_width = 12.2;

board_height = 1.4;

roundness = 1;

//color([0.8,0.8,0.8]) ldp_lid(3.5);
//ldp_board(-1.75);
translate([0,0,-0.5]) ldp_case();
//ldp_endcap(0);

// m2.5 bolt
//translate([0,-13.5,-6.25]) color([1,0,0]) cylinder(d=2.5, h=12, $fn=36);

// m2.5 nut
//translate([0,-13.5,-6.55]) rotate(30) color([1,0,0]) cylinder(d=5.8, h=2.4, $fn=6);

module ldp_endcap(o)
{
		 
	translate([0,-17.75,-1]) rotate([90,0,0])
		linear_extrude(1)
			text("BLAUSTAHL", size=2, halign="center", font="Lato");
	
	difference() {
		union() {
			translate([0,-13.5,o-0.775]) color([0,0,1]) cylinder(d=6, h=4, $fn=36);
			translate([0,-13.5,o-3.225]) color([0,0,1]) cylinder(d=4, h=3.5, $fn=36);
			translate([0,-13.5,o-3.225]) color([0,0,1]) cylinder(d=8, h=1, $fn=36);
			translate([0,-16,0]) cube([16,5,6.45], center=true);
		}
		translate([0,-13.5,-4.25]) color([1,0,0]) cylinder(d=3, h=8, $fn=36);
		
	}
}

module ldp_board(o)
{
	
	difference() {
		union() {
			translate([0,2,o])
				color([0.0,0.8,0.0]) cube([16, 30, 1.6], center=true);
			translate([0,23,o+1.45])
				color([0.9,0.9,0.9]) cube([12, 15, 4.5], center=true);
		}
		translate([0,-13,-10]) cylinder(d=4.2, h=100);
	}
	
}

module ldp_case()
{
	
	difference() {
		
		minkowski() {
			cube([box_width-(roundness*2),box_length-(roundness*2),box_height-(roundness*2)],
				center=true);
			sphere(roundness);
		}
	
		translate([0,-5,(box_height-cutout_height)/2-2.5])
			cube([cutout_width,cutout_length,cutout_height], center=true);

		// USB cutout
		translate([0,15,0.25])
			cube([cutout_usb_width,10,4.6], center=true);

		// bolt hole
		translate([0,-13.5,-5]) cylinder(d=2.75, h=100, $fn=36);
		
		// nut cutout
		translate([0,-13.5,-6.75]) rotate(30) cylinder(d=6.25, h=2.5, $fn=6);
		
		// lid notch cutout
		translate([0,-13.5,5.25]) cylinder(d=5, h=100);

	}

}