import time
import itertools
import contextlib

import PyOpenColorIO as OCIO

import rv
from rv.commands import setStringProperty, setIntProperty, setFloatProperty


@contextlib.contextmanager
def timer(msg):
    start = time.time()
    yield
    end = time.time()
    print "%s: %.02fms" % (msg, (end-start)*1000)


def set_lut(proc, nodename):
    """Given an PyOpenColorIO.Processor instance, create a LUT and
    sets it for the specified node
    """

    if proc.isNoOp():
        return

    if hasattr(proc, "hasChannelCrosstalk"):
        # Determine if transform has crosstalk, and switch 1D/3D based on this
        crosstalk = proc.hasChannelCrosstalk()
    else:
        # Assume crosstalk in lieu of latest OCIO Python bindings
        crosstalk = True

    # TODO: Maybe allow configuration of LUT sizes?
    size_1d = 2048
    size_3d = 32

    if crosstalk:
        _set_lut_3d(proc = proc, nodename = nodename, size = size_3d)
    else:
        _set_lut_1d(proc = proc, nodename = nodename, size = size_1d)

    # TODO: Can also set nodename.lut.inMatrix - could maybe avoid creating a
    # 3D LUT for linear transforms


def _set_lut_3d(proc, nodename, size = 32):
    # FIXME: This clips with >1 scene-linear values, use allocation
    # vars

    # Make noop cube
    size_minus_one = float(size-1)
    one_axis = (x/size_minus_one for x in range(size))
    cube_raw = itertools.product(one_axis, repeat=3)

    # Unpack and fix ordering, by turning [(0, 0, 0), (0, 0, 1), ...]
    # into [0, 0, 0, 1, 0, 0] as generator
    cube_raw = (item for sublist in cube_raw for item in sublist[::-1])

    # Apply transform
    cube_processed = proc.applyRGB(cube_raw)

    # Set LUT type and size, then LUT values
    setStringProperty("%s.lut.type" % nodename, ["RGB"], False)
    setIntProperty("%s.lut.size" % nodename, [size, size, size], False)
    setFloatProperty("%s.lut.lut" % nodename, cube_processed, True)

    # Activate
    setIntProperty("%s.lut.active" % nodename, [1], False)


def _set_lut_1d(proc, nodename, size = 1024):
    # TODO: Use allocation vars also

    # Make noop ramp
    def gen_noop_ramp(size):
        size_minus_one = float(size-1)
        for x in range(size):
            val = x/size_minus_one
            for i in range(3):
                yield val

    ramp_raw = gen_noop_ramp(size = size)

    # Apply transform
    # TODO: Make applyRGB accept an iterable, rather than requiring a
    # list, to avoid making the intermediate list
    ramp_transformed = proc.applyRGB(ramp_raw)

    # Set LUT type and size, then LUT values
    setStringProperty("%s.lut.type" % nodename, ["RGB"], False)
    setIntProperty("%s.lut.size" % nodename, [size], False)
    setFloatProperty("%s.lut.lut" % nodename, ramp_transformed, True)

    # Activate
    setIntProperty("%s.lut.active" % nodename, [1], False)


def set_noop(nodename):
    """Just ensure sure the LUT is deactivated, in case source changes
    from switch from a non-noop to a noop processor
    """

    is_active = False
    setIntProperty("%s.lut.active" % nodename, [is_active], False)


class OcioRv(rv.rvtypes.MinorMode):

    def cycle_display(self, forward = False, backward = False):
        if not (forward or backward):
            raise ValueError("Must specify either forward to backward")

        if forward:
            delta = 1
        elif backward:
            delta = -1

        all_disp = sorted(self.get_views().keys()) # TODO: avoid losing order from config?
        curindex = all_disp.index(self.active_display)
        newindex = (curindex + delta) % len(all_disp)

        self.active_display = all_disp[newindex]

        if self.active_view not in self.get_views()[self.active_display]:
            # Current view doesn't exist in new display, use default instead
            cfg = self.get_cfg()
            self.active_view = cfg.getDefaultView(cfg.getDefaultDisplay())
        else:
            # Keep current view
            pass

        self.refresh()

    def cycle_view(self, forward = False, backward = False):
        if not (forward or backward):
            raise ValueError("Must specify either forward to backward")

        if forward:
            delta = 1
        elif backward:
            delta = -1

        avail = self.get_views()[self.active_display]

        cur = self.active_view
        curindex = avail.index(cur)
        newindex = (curindex + delta) % len(avail)
        self.active_view = avail[newindex]

        self.refresh()

    def get_cfg(self):
        return OCIO.GetCurrentConfig()

    def refresh(self):
        """Refresh LUT on all sources
        """

        rv.extra_commands.displayFeedback(
            "Changing display to %s/%s" % (self.active_display, self.active_view),
            2.0, # timeout
            )

        for src in rv.commands.nodesOfType("RVSource"):
            path = rv.commands.getStringProperty(
                "%s.media.movie" % src,
                0, 99999999)[0] # defaultish arguments
            self._config_source(src = src, srcpath = path)

    def source_setup(self, event):
        # Event content example:
        # sourceGroup000001_source;;RVSource;;/path/to/myimg.exr

        args = event.contents().split(";;")
        src = args[0]
        srcpath = args[2]

        self._config_source(src = src, srcpath = srcpath)

        # TODO: Is this right? Allows the default source-setup to run
        event.reject()

    def _config_source(self, src, srcpath):
        filelut_node = rv.extra_commands.associatedNode("RVColor", src)
        looklut_node = rv.extra_commands.associatedNode("RVLookLUT", src)

        # Get config, and input colorspace
        cfg = self.get_cfg()

        # FIXME: Need a way to customise this per-facility without
        # modifying this file (try: import ociorv_custom_stuff ?)
        inspace = cfg.parseColorSpaceFromString(srcpath)

        # Create display transform to test for noop transform
        test_transform = OCIO.DisplayTransform()
        test_transform.setInputColorSpaceName(inspace)

        test_transform.setDisplay(self.active_display)
        test_transform.setView(self.active_view)

        try:
            test_proc = cfg.getProcessor(test_transform)
        except OCIO.Exception, e:
            print "INFO: Cannot create test OCIODisplay for %s - display LUT disabled. OCIO message was %s" % (
                inspace,
                e)
            set_noop(nodename = filelut_node)
            set_noop(nodename = looklut_node)
            return

        if test_proc.isNoOp():
            print "INFO: Source is NoOp"
            set_noop(nodename = filelut_node)
            set_noop(nodename = looklut_node)
            return

        # Input -> scene-linear processor. Allow grading controls etc
        # to work as expected
        try:
            input_proc = cfg.getProcessor(inspace, OCIO.Constants.ROLE_SCENE_LINEAR)
        except OCIO.Exception, e:
            # TODO: This mostly catches "3D Luts can only be applied
            # in the forward direction.", could handle this better
            print "INFO: Cannot linearise %s - display LUT disabled. OCIO message was %s" % (
                inspace,
                e)
            set_noop(nodename = filelut_node)
            set_noop(nodename = looklut_node)
            return

        # Scene-linear -> output display transform
        transform = OCIO.DisplayTransform()
        transform.setDisplay(self.active_display)
        transform.setView(self.active_view)

        transform.setInputColorSpaceName(OCIO.Constants.ROLE_SCENE_LINEAR)

        try:
            output_proc = cfg.getProcessor(transform)
        except OCIO.Exception, e:
            print "INFO: Cannot apply scene-linear to %s - OCIO error was %s" % (
                srcpath,
                e)
            set_noop(nodename = filelut_node)
            set_noop(nodename = looklut_node)
            return

        # LUT to be applied to input file, before any grading etc
        set_lut(proc = input_proc, nodename = filelut_node)

        # LUT after grading etc performed
        set_lut(proc = output_proc, nodename = looklut_node)

        # Update LUT (think this makes RV send the LUT-stuff to the GPU?)
        rv.commands.updateLUT()

    def set_view(self, display, view):
        """Set view, and update all sources
        """
        self.active_display = display
        self.active_view = view
        self.refresh()

    def get_views(self):
        """Return [("sRGB", "Film"), ("sRGB", "Log"), ...]
        """

        ret = {}

        cfg = self.get_cfg()
        for a_display in cfg.getActiveDisplays().split(", "):
            for a_view in cfg.getViews(a_display):
                if a_view not in cfg.getActiveViews():
                    continue

                ret.setdefault(a_display, []).append(a_view)

        return ret

    def __init__(self):
        rv.rvtypes.MinorMode.__init__(self)

        mode_name = "ociorv-mode"

        global_bindings = [
            ("key-down--s", lambda e: self.cycle_display(forward=True), "cycle current view forward"),
            ("key-down--S", lambda e: self.cycle_display(backward=True), "cycle current view forward"),
            ("key-down--d", lambda e: self.cycle_view(forward=True), "cycle current view forward"),
            ("key-down--D", lambda e: self.cycle_view(backward=True), "cycle current view forward"),
            ("new-source", self.source_setup, "OCIO source setup"),
            ]

        local_bindings = []

        # Create [("sRGB/Film", self.set_menu(...)), ("sRGB/Log", ...)]
        views_menu = []
        for cur_disp, all_views in self.get_views().items():
            for cur_view in all_views:
                def set_view(event, cur_disp = cur_disp, cur_view = cur_view):
                    self.set_view(display = cur_disp, view = cur_view)

                def menu_is_active(cur_disp = cur_disp, cur_view = cur_view):
                    if (cur_disp, cur_view) == (self.active_display, self.active_view):
                        return rv.commands.CheckedMenuState
                    else:
                        return rv.commands.NeutralMenuState

                mi = (
                    "%s/%s" % (cur_disp, cur_view), # label
                    set_view, # actionHook
                    "", # key
                    menu_is_active, # stateHook
                    )
                views_menu.append(mi)

            views_menu.append(("_", lambda e: 0))

        # Construct top-level menu
        menu = [("OCIO", [
                    ("Display/Views", views_menu)])]


        # Set default display/view
        cfg = self.get_cfg()
        self.active_display = cfg.getDefaultDisplay()
        self.active_view = cfg.getDefaultView(cfg.getDefaultDisplay())

        # Initialise mode
        self.init(mode_name,
                  global_bindings,
                  local_bindings,
                  menu)


def createMode():
    return OcioRv()
