# iperf Test Plug — Fusion 360 housing script
# Waveshare ESP32-S3-ETH (23 × 72 mm) + SH1106 1.3" OLED (35.6 × 33.6 mm PCB)
# Version: 4
#
# Usage: Fusion 360 → Tools → Scripts and Add-Ins → Run → select this file
# Creates two components: iperf_body and iperf_lid, placed side-by-side.
#
# Coordinate system (Fusion 360 default, Z up):
#   X — housing width
#   Y — housing length  (RJ45 at y=0, USB-C at y=outer_l)
#   Z — housing height  (floor at z=0, open top at z=body_h)

import adsk.core, adsk.fusion, traceback, math

def run(context):
    ui = None
    try:
        app  = adsk.core.Application.get()
        ui   = app.userInterface
        des  = adsk.fusion.Design.cast(app.activeProduct)
        root = des.rootComponent

        m = 0.1  # mm → cm (Fusion 360 internal unit)

        # ── Parameters (all in mm) ────────────────────────────────────────────
        board_w  = 23.0   # PCB width
        board_l  = 72.0   # PCB length
        board_t  =  1.6   # PCB thickness
        standoff =  3.5   # support post height (must exceed pin-header tail ~3 mm)

        rj45_w, rj45_h, rj45_z = 16.2, 14.0, 0.0   # RJ45 opening + z offset from PCB bottom
        usbc_w, usbc_h, usbc_z =  9.5,  3.5, 0.5   # USB-C opening + z offset

        dm_w  = 33.6   # display module PCB width  (X direction in housing)
        dm_l  = 35.6   # display module PCB length (Y direction in housing)
        dm_t  =  1.5   # display PCB thickness
        dm_hd =  3.0   # mounting hole diameter
        dm_hm =  2.5   # hole centre from each PCB edge
        scr_w = 29.4   # visible OLED screen width
        scr_h = 14.7   # visible OLED screen height

        wall     = 2.0
        floor_t  = 1.5
        corner_r = 2.0

        lid_top  = 3.0   # lid plate thickness
        post_h   = 5.0   # display mounting post height (into body cavity)
        rim_ins  = 5.0   # press-fit rim insertion depth
        rim_t    = 1.5   # rim wall thickness
        rim_lc   = 0.25  # press-fit clearance each side

        # ── Derived ───────────────────────────────────────────────────────────
        inner_w = dm_w + 2.4                            # 36.0 — 1.2 mm side clearance
        inner_l = board_l + 1.0                         # 73.0
        pcb_bot = floor_t + standoff                    # 5.0 — z of PCB bottom
        inner_h = math.ceil(standoff + board_t + rj45_h + 0.5)  # 20
        outer_w = inner_w + 2 * wall                    # 40.0
        outer_l = inner_l + 2 * wall                    # 77.0
        body_h  = floor_t + inner_h                     # 21.5
        bx      = (inner_w - board_w) / 2              # 6.5 — board X offset in cavity
        dm_xo   = (inner_w - dm_w) / 2                 # 1.2 — display X offset in cavity
        dm_yo   = 8.0                                   # display Y offset from RJ45 end
        scr_xo  = dm_xo + (dm_w - scr_w) / 2
        scr_yo  = dm_yo + (dm_l - scr_h) / 2

        # Display mounting hole centres (relative to inner cavity origin)
        holes = [
            (wall + dm_xo + dm_hm,        wall + dm_yo + dm_hm       ),
            (wall + dm_xo + dm_w - dm_hm, wall + dm_yo + dm_hm       ),
            (wall + dm_xo + dm_hm,        wall + dm_yo + dm_l - dm_hm),
            (wall + dm_xo + dm_w - dm_hm, wall + dm_yo + dm_l - dm_hm),
        ]

        # ── Helper: draw rounded rectangle in a sketch ────────────────────────
        # All 4 arcs sweep CCW (+π/2); traversal: bottom→BR→right→TR→top→TL→left→BL
        def add_rrect(sketch, x, y, w, l, r):
            ln  = sketch.sketchCurves.sketchLines
            arc = sketch.sketchCurves.sketchArcs
            P   = adsk.core.Point3D.create
            sw  = math.pi / 2
            ln.addByTwoPoints(P((x+r)*m, y*m, 0),     P((x+w-r)*m, y*m, 0)    )  # bottom
            ln.addByTwoPoints(P((x+w)*m, (y+r)*m, 0), P((x+w)*m, (y+l-r)*m, 0))  # right
            ln.addByTwoPoints(P((x+w-r)*m,(y+l)*m,0), P((x+r)*m,  (y+l)*m,  0))  # top
            ln.addByTwoPoints(P(x*m, (y+l-r)*m, 0),   P(x*m,  (y+r)*m,   0)    )  # left
            arc.addByCenterStartSweep(P((x+w-r)*m,(y+r)*m,  0),P((x+w-r)*m,y*m,      0),sw) # BR
            arc.addByCenterStartSweep(P((x+w-r)*m,(y+l-r)*m,0),P((x+w)*m, (y+l-r)*m,0),sw) # TR
            arc.addByCenterStartSweep(P((x+r)*m,  (y+l-r)*m,0),P((x+r)*m, (y+l)*m,  0),sw) # TL
            arc.addByCenterStartSweep(P((x+r)*m,  (y+r)*m,  0),P(x*m,     (y+r)*m,  0),sw) # BL

        # ── Helper: extrude a profile ─────────────────────────────────────────
        def extrude(comp, prof, dist, op, direction=adsk.fusion.ExtentDirections.PositiveExtentDirection):
            exs = comp.features.extrudeFeatures
            inp = exs.createInput(
                prof if not isinstance(prof, list) else prof[0],
                op)
            if isinstance(prof, list):
                col = adsk.core.ObjectCollection.create()
                for p in prof: col.add(p)
                inp = exs.createInput(col, op)
            inp.setOneSideExtent(
                adsk.fusion.DistanceExtentDefinition.create(
                    adsk.core.ValueInput.createByReal(dist * m)),
                direction)
            return exs.add(inp)

        def extrude_profiles(comp, prof_list, dist, op,
                             direction=adsk.fusion.ExtentDirections.PositiveExtentDirection):
            col = adsk.core.ObjectCollection.create()
            for p in prof_list: col.add(p)
            exs = comp.features.extrudeFeatures
            inp = exs.createInput(col, op)
            inp.setOneSideExtent(
                adsk.fusion.DistanceExtentDefinition.create(
                    adsk.core.ValueInput.createByReal(dist * m)),
                direction)
            return exs.add(inp)

        NEG = adsk.fusion.ExtentDirections.NegativeExtentDirection
        POS = adsk.fusion.ExtentDirections.PositiveExtentDirection
        NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
        CUT = adsk.fusion.FeatureOperations.CutFeatureOperation
        JOI = adsk.fusion.FeatureOperations.JoinFeatureOperation

        # ════════════════════════════════════════════════════════════════════
        # BODY
        # ════════════════════════════════════════════════════════════════════
        bodyOcc  = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        bc       = bodyOcc.component
        bc.name  = "iperf_body"
        bsk      = bc.sketches
        bft      = bc.features

        # 1 — Outer shell: rounded rectangle extruded to body_h
        sk1 = bsk.add(bc.xYConstructionPlane)
        sk1.name = "outer_profile"
        add_rrect(sk1, 0, 0, outer_w, outer_l, corner_r)
        e1 = extrude(bc, sk1.profiles.item(0), body_h, NEW)
        e1.name = "body_shell"

        # 2 — Inner cavity: rectangle cut from top face downward by inner_h
        #     Leaves floor_t = 1.5 mm floor
        top_face = e1.endFaces.item(0)
        sk2 = bsk.add(top_face)
        sk2.name = "inner_cavity"
        sk2.sketchCurves.sketchLines.addTwoPointRectangle(
            adsk.core.Point3D.create(wall*m, wall*m, 0),
            adsk.core.Point3D.create((wall+inner_w)*m, (wall+inner_l)*m, 0))
        e2 = extrude(bc, sk2.profiles.item(0), inner_h, CUT, NEG)
        e2.name = "inner_cavity"

        # 3 — RJ45 cutout: slot on front face (y=0 wall, 2 mm thick)
        #     Approach: sketch on XY-offset plane at z=rj45_z_abs, draw a rectangle
        #     that spans x=rj45 width and y=-1..wall+2 (straddles the front wall),
        #     then extrude CUT in +Z by rj45_h.  Uses only the +Z axis which is
        #     confirmed working — avoids XZ-plane coordinate-mapping ambiguity.
        bplanes = bc.constructionPlanes
        rj45_x   = (outer_w - rj45_w) / 2
        rj45_z_abs = pcb_bot + rj45_z   # z of bottom edge of opening = 5.0 mm

        rj45PlaneInp = bplanes.createInput()
        rj45PlaneInp.setByOffset(bc.xYConstructionPlane,
            adsk.core.ValueInput.createByReal(rj45_z_abs * m))
        rj45Plane = bplanes.add(rj45PlaneInp)
        rj45Plane.name = "rj45_base_plane"

        sk3 = bsk.add(rj45Plane)
        sk3.name = "rj45_profile"
        sk3.sketchCurves.sketchLines.addTwoPointRectangle(
            adsk.core.Point3D.create( rj45_x        *m, -1      *m, 0),
            adsk.core.Point3D.create((rj45_x+rj45_w)*m, (wall+2)*m, 0))
        e3 = extrude(bc, sk3.profiles.item(0), rj45_h, CUT, POS)
        e3.name = "rj45_cutout"

        # 4 — USB-C cutout: same technique on back face (y=outer_l wall)
        usbc_x   = (outer_w - usbc_w) / 2
        usbc_z_abs = pcb_bot + usbc_z   # z of bottom edge of opening = 5.5 mm

        usbcPlaneInp = bplanes.createInput()
        usbcPlaneInp.setByOffset(bc.xYConstructionPlane,
            adsk.core.ValueInput.createByReal(usbc_z_abs * m))
        usbcPlane = bplanes.add(usbcPlaneInp)
        usbcPlane.name = "usbc_base_plane"

        sk4 = bsk.add(usbcPlane)
        sk4.name = "usbc_profile"
        sk4.sketchCurves.sketchLines.addTwoPointRectangle(
            adsk.core.Point3D.create( usbc_x        *m, (outer_l-wall-2)*m, 0),
            adsk.core.Point3D.create((usbc_x+usbc_w)*m, (outer_l+1)    *m, 0))
        e4 = extrude(bc, sk4.profiles.item(0), usbc_h, CUT, POS)
        e4.name = "usbc_cutout"

        # 5 — Board standoff posts: 4 circles on floor plane, extruded up
        floorInp = bplanes.createInput()
        floorInp.setByOffset(bc.xYConstructionPlane,
            adsk.core.ValueInput.createByReal(floor_t * m))
        floorPlane = bplanes.add(floorInp)
        floorPlane.name = "floor_plane"

        sk5 = bsk.add(floorPlane)
        sk5.name = "post_circles"
        post_r = 1.5
        inset  = 3.0
        circ   = sk5.sketchCurves.sketchCircles
        for px in [wall+bx+inset, wall+bx+board_w-inset]:
            for py in [wall+inset, wall+inner_l-inset]:
                circ.addByCenterRadius(
                    adsk.core.Point3D.create(px*m, py*m, 0), post_r*m)

        post_profs = [sk5.profiles.item(i) for i in range(sk5.profiles.count)]
        e5 = extrude_profiles(bc, post_profs, standoff, JOI, POS)
        e5.name = "standoff_posts"

        # 6 — Board lateral guides: thin rails on inner walls to locate board in X
        sk6 = bsk.add(floorPlane)
        sk6.name = "guide_rails"
        guide_h_z = standoff + board_t + 1.0
        guide_w   = bx - 0.3
        guide_l   = 12.0
        guide_y   = wall + (inner_l - guide_l) / 2
        ln6 = sk6.sketchCurves.sketchLines
        # Left rail
        ln6.addTwoPointRectangle(
            adsk.core.Point3D.create(wall*m, guide_y*m, 0),
            adsk.core.Point3D.create((wall+guide_w)*m, (guide_y+guide_l)*m, 0))
        # Right rail
        ln6.addTwoPointRectangle(
            adsk.core.Point3D.create((wall+inner_w-guide_w)*m, guide_y*m, 0),
            adsk.core.Point3D.create((wall+inner_w)*m, (guide_y+guide_l)*m, 0))
        rail_profs = [sk6.profiles.item(i) for i in range(sk6.profiles.count)]
        e6 = extrude_profiles(bc, rail_profs, guide_h_z, JOI, POS)
        e6.name = "board_guides"

        # ════════════════════════════════════════════════════════════════════
        # LID — placed 10 mm to the right of body for preview
        # ════════════════════════════════════════════════════════════════════
        lid_x_offset = outer_w + 10.0

        lidMatrix = adsk.core.Matrix3D.create()
        lidMatrix.translation = adsk.core.Vector3D.create(lid_x_offset*m, 0, 0)
        lidOcc  = root.occurrences.addNewComponent(lidMatrix)
        lc_comp = lidOcc.component
        lc_comp.name = "iperf_lid"
        lsk = lc_comp.sketches
        lft = lc_comp.features
        lpl = lc_comp.constructionPlanes

        # 1 — Top plate: rounded rectangle extruded by lid_top
        lsk1 = lsk.add(lc_comp.xYConstructionPlane)
        lsk1.name = "lid_outer_profile"
        add_rrect(lsk1, 0, 0, outer_w, outer_l, corner_r)
        le1 = extrude(lc_comp, lsk1.profiles.item(0), lid_top, NEW)
        le1.name = "lid_plate"

        # 2 — Press-fit rim: hollow rectangle extruded downward from underside
        #     Clearance rim_lc on each side; rim wall thickness rim_t
        rim_ox = wall + rim_lc           # outer rim X start
        rim_oy = wall + rim_lc           # outer rim Y start
        rim_ow = inner_w - 2*rim_lc      # outer rim width
        rim_ol = inner_l - 2*rim_lc      # outer rim length
        rim_ix = rim_ox + rim_t          # inner cutout X start
        rim_iy = rim_oy + rim_t
        rim_iw = rim_ow - 2*rim_t
        rim_il = rim_ol - 2*rim_t

        bot_face = le1.startFaces.item(0)
        lsk2 = lsk.add(bot_face)
        lsk2.name = "rim_profile"
        ln2 = lsk2.sketchCurves.sketchLines
        # Outer rim rectangle
        ln2.addTwoPointRectangle(
            adsk.core.Point3D.create(rim_ox*m, rim_oy*m, 0),
            adsk.core.Point3D.create((rim_ox+rim_ow)*m, (rim_oy+rim_ol)*m, 0))
        # Inner cutout rectangle (creates the hollow cross-section)
        ln2.addTwoPointRectangle(
            adsk.core.Point3D.create(rim_ix*m, rim_iy*m, 0),
            adsk.core.Point3D.create((rim_ix+rim_iw)*m, (rim_iy+rim_il)*m, 0))
        # The profile between the two rectangles is the rim cross-section
        # Fusion 360 auto-selects the enclosed region between outer and inner rects
        rim_prof = None
        for i in range(lsk2.profiles.count):
            p = lsk2.profiles.item(i)
            # The rim profile area ≈ rim_ow*rim_ol - rim_iw*rim_il
            rim_area = p.areaProperties().area
            expected = (rim_ow * rim_ol - rim_iw * rim_il) * m * m
            if abs(rim_area - expected) < expected * 0.2:
                rim_prof = p
                break
        if rim_prof is None:
            rim_prof = lsk2.profiles.item(0)  # fallback

        le2 = extrude(lc_comp, rim_prof, rim_ins, JOI, NEG)
        le2.name = "press_fit_rim"

        # 3 — Screen window: rectangle cut through lid plate
        top_face_lid = le1.endFaces.item(0)
        lsk3 = lsk.add(top_face_lid)
        lsk3.name = "screen_window"
        lsk3.sketchCurves.sketchLines.addTwoPointRectangle(
            adsk.core.Point3D.create((wall+scr_xo)*m, (wall+scr_yo)*m, 0),
            adsk.core.Point3D.create((wall+scr_xo+scr_w)*m, (wall+scr_yo+scr_h)*m, 0))
        le3 = extrude(lc_comp, lsk3.profiles.item(0), lid_top + 0.1, CUT, NEG)
        le3.name = "screen_window"

        # 4 — Display mounting posts: circles on underside, extruded downward (into body)
        lsk4 = lsk.add(bot_face)
        lsk4.name = "post_circles"
        lcirc = lsk4.sketchCurves.sketchCircles
        post_outer_r = dm_hd / 2 + 1.2   # post outer radius (for M3 self-tapping)
        for hx, hy in holes:
            lcirc.addByCenterRadius(
                adsk.core.Point3D.create(hx*m, hy*m, 0), post_outer_r*m)

        post_lid_profs = [lsk4.profiles.item(i) for i in range(lsk4.profiles.count)]
        le4 = extrude_profiles(lc_comp, post_lid_profs, post_h, JOI, NEG)
        le4.name = "display_posts"

        # 5 — M3 pilot holes through posts (2.5 mm dia, self-tapping)
        lsk5 = lsk.add(bot_face)
        lsk5.name = "screw_holes"
        lcirc5 = lsk5.sketchCurves.sketchCircles
        for hx, hy in holes:
            lcirc5.addByCenterRadius(
                adsk.core.Point3D.create(hx*m, hy*m, 0), 1.25*m)  # r = 1.25 mm

        hole_profs = [lsk5.profiles.item(i) for i in range(lsk5.profiles.count)]
        le5 = extrude_profiles(lc_comp, hole_profs, post_h + lid_top, CUT, NEG)
        le5.name = "screw_pilot_holes"

        # 6 — Label embossed on top (optional — comment out if text is slow)
        #     Use a construction plane at z=lid_top rather than top_face_lid,
        #     which is stale after the screen-window cut modified the face topology.
        labelPlaneInp = lpl.createInput()
        labelPlaneInp.setByOffset(lc_comp.xYConstructionPlane,
            adsk.core.ValueInput.createByReal(lid_top * m))
        labelPlane = lpl.add(labelPlaneInp)
        label_sk = lsk.add(labelPlane)
        label_sk.name = "label"
        txt = label_sk.sketchTexts
        txtInp = txt.createInput2("iperf test plug", 3.5 * m)
        txtInp.setAsMultiLine(
            adsk.core.Point3D.create((outer_w/2)*m, (outer_l - wall - 7)*m, 0),
            adsk.core.Point3D.create((outer_w/2 + 30)*m, (outer_l - wall - 7)*m, 0),
            adsk.fusion.HorizontalAlignments.CenterHorizontalAlignment,
            adsk.fusion.VerticalAlignments.MiddleVerticalAlignment, 0)
        txt.add(txtInp)
        label_profs = [label_sk.profiles.item(i) for i in range(label_sk.profiles.count)]
        if label_profs:
            le6 = extrude_profiles(lc_comp, label_profs, 0.4, CUT, NEG)
            le6.name = "label_engraved"

        # ── Done ─────────────────────────────────────────────────────────────
        des.designType = adsk.fusion.DesignTypes.ParametricDesignType
        ui.messageBox(
            f"Housing created.\n\n"
            f"Body:  {outer_w:.0f} × {outer_l:.0f} × {body_h:.1f} mm\n"
            f"Lid:   {outer_w:.0f} × {outer_l:.0f} × {lid_top+post_h:.1f} mm\n"
            f"Total: {outer_w:.0f} × {outer_l:.0f} × {body_h+lid_top:.1f} mm\n\n"
            f"Export each component separately for printing.\n"
            f"Assembly: M3 × 6 self-tapping screws for display.")

    except:
        if ui:
            ui.messageBox("Script failed:\n" + traceback.format_exc())
