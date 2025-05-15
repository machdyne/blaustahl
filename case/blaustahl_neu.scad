/*
 * Blaustahl Case Neu Edition
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * required hardware:
 *  - 1 x M2.5 x 10mm countersunk bolt
 *  - 1 x M2.5 nut
 *
 */
 
$fn = 30;

box_width = 19;
box_length = 37;
box_height = 10.5;
box_thickness = 2.0;

lid_thickness = 3;

cutout_width = 16.10;
cutout_length = 44;
cutout_height = 5.5;

cutout_usb_width = 12;

board_height = 1.4;

roundness = 2;

toladj = 0.25;

ldp_board(-0.5);
translate([0,0,-0.5]) ldp_case();
translate([0,0,-0.25]) ldp_endcap();

// m2.5 bolt
//translate([0,-13.5,-6.25]) color([1,0,0]) cylinder(d=2.5, h=12, $fn=36);

// m2.5 nut
//translate([0,-13.5,-6.55]) rotate(30) color([1,0,0]) cylinder(d=5.8, h=2.4, $fn=6);

module ldp_endcap()
{
    		 	
	difference() {
		union() {
			translate([0,-13.5,-0.5]) color([0,0,1]) cylinder(d=6, h=3.5, $fn=36);
			translate([0,-15.75,-0.25]) cube([16.1,5.5,6.5], center=true);
		}
		translate([0,-13.5,-4.25]) color([1,0,0]) cylinder(d=3, h=8, $fn=36);
        color([1,0,0]) translate([0,0,-5.5]) cube([6.5,100,6], center=true);

	}
}

module ldp_board(o)
{

	difference() {
		union() {
			translate([0,2,o])
				color([0.0,0.8,0.0]) cube([16, 30, 1.6], center=true);
			translate([0,23,o+1])
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
        
		translate([0,-5,(box_height-cutout_height)/2-2])
			cube([cutout_width+toladj,cutout_length,cutout_height], center=true);

		translate([-5.675,-5,(box_height-cutout_height)/2-3])
			cube([5,cutout_length,cutout_height], center=true);
		
		translate([5.675,-5,(box_height-cutout_height)/2-3])
			cube([5,cutout_length,cutout_height], center=true);

		// USB cutout
        translate([0,15,0.8])
            cube([cutout_usb_width+toladj,10,4.6+toladj], center=true);
        
		// bolt hole
		translate([0,-13.5,-5]) cylinder(d=2.75+toladj, h=100, $fn=36);
		
		// nut cutout
		translate([0,-13.5,-5.75]) rotate(30) cylinder(d=6+toladj, h=2.5, $fn=6);
    
        // lid notch cutout
        translate([0,-13.5,4.5]) cylinder(d=5+toladj, h=100);

//        translate([-1.5,2.5,5.1]) rotate([0,0,270])
//            linear_extrude(0.3)
//                text("BLAUSTAHL", size=3, halign="center", font="Lato Black");

	}

}