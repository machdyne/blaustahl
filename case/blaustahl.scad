/*
 * Blaustahl Case
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * required hardware:
 *  - 1 x M2.5 x 10mm countersunk bolt
 *  - 1 x M2.5 nut
 *
 */
 
$fn = 30;

box_width = 18;
box_length = 37;
box_height = 11;
box_thickness = 2.0;

lid_thickness = 3;

cutout_width = 16.10;
cutout_length = 33.60;
cutout_height = 8.5;

cutout_usb_width = 12;

board_height = 1.4;

roundness = 1;

color([0.8,0.8,0.8]) ldp_lid(3.5);
ldp_board(-1.75);
translate([0,0,-0.5]) ldp_case();
ldp_spacer(0);

// m2.5 bolt
//translate([0,-13,-4.25]) color([1,0,0]) cylinder(d=2.5, h=8, $fn=36);

// m2.5 nut
//translate([0,-13,-5.55]) color([1,0,0]) cylinder(d=5.8, h=2.4, $fn=6);

module ldp_spacer(o)
{
	difference() {
		union() {
			translate([0,-13.5,o-0.75]) color([0,0,1]) cylinder(d=6, h=3, $fn=36);
			translate([0,-13.5,o-2.3]) color([0,0,1]) cylinder(d=4, h=2.5, $fn=36);
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

	//color([0,1,0]) cube([box_width,box_length,box_height], center=true);
	
	difference() {

		
		minkowski() {
			cube([box_width-(roundness*2),box_length-(roundness*2),box_height-(roundness*2)],
				center=true);
			sphere(roundness);
		}
	
		translate([0,0,(box_height-cutout_height)/2])
			cube([cutout_width,cutout_length,cutout_height], center=true);

		// USB cutout
		translate([0,15,3])
			cube([cutout_usb_width,10,10], center=true);

		// bolt hole
		translate([0,-13.5,-5]) cylinder(d=2.75, h=100, $fn=36);
		
		// nut cutout
		translate([0,-13.5,-5.5]) rotate(30) cylinder(d=6.25, h=2.5, $fn=6);
		
		// lid notch cutout
		translate([0,box_length/2-1.75,3.5])
			rotate([0,90,0]) cylinder(d=2.25, h=14.25, center=true);
				
	}

	// board support
	translate([0,0,-3])
		cube([16,16,2.25], center=true);

	difference() {
		
		translate([0,-10,-2.75])
			cube([8,15,1.75], center=true);		

		// bolt hole
		translate([0,-13.5,-5]) cylinder(d=2.75, h=100, $fn=36);
		
		// nut cutout
		translate([0,-13.5,-5.25]) rotate(30) cylinder(d=6.25, h=2.5, $fn=6);
	}
	
}

module ldp_lid(o)
{
	/*
	rotate(270)
		translate([-3,-2.25/2,o+0.25])
			linear_extrude(1.25)
				resize([25,2.25,1.25]) text("BLAUSTAHL", size=10, spacing=1.75, halign="center",
					font="Liberation Sans:style=Bold");
	*/
	
	union() {
		translate([0,box_length/2-1.5,o])
			cube([11.75,1,lid_thickness], center=true);
		translate([-5.875,box_length/2-1.25,o+0.25])
			rotate([0,90,0]) cylinder(d=2.5, h=11.75);
		translate([0,box_length/2-1,o-0.5])
			cube([11.75,2,2], center=true);
	}
			
	difference() {

		
		union() {
			
			translate([0,0,o]) {
				cube([box_width-box_thickness,box_length-box_thickness-1.5,lid_thickness], center=true);
			}
			
			translate([0,box_length/2-1.75,o-0.5])
				rotate([0,90,0]) cylinder(d=2, h=14, center=true);

		}
		/*
		rotate(270)
			translate([-2.5,-2/2,o+1])
				linear_extrude(1.25)
					resize([25,2,1.25]) text("BLAUSTAHL", size=10, spacing=1.75, halign="center",
						font="Liberation Sans:style=Bold");
		*/
		translate([0,-13.5,3.5]) cylinder(d=5, h=100);
		translate([0,-13.5,-5]) cylinder(d=2.75, h=100);
		
	}
	
}
