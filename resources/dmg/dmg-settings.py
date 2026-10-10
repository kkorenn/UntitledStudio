# dmgbuild settings for the Untitled Studio installer window.
# Used by scripts/package-macos.sh:  dmgbuild -s resources/dmg/dmg-settings.py -D app=... -D background=... -D icon=...
# The GPL licence ships inside the app (Contents/Resources/licenses, also shown in Help > About).
import os

app = defines["app"]
app_name = os.path.basename(app)

format = "UDZO"
size = None
files = [app]
symlinks = {"Applications": "/Applications"}
icon = defines["icon"]                 # disk icon on the desktop / in Finder's sidebar
background = defines["background"]     # 1x + 2x TIFF, 660x420 points

window_rect = ((200, 140), (660, 420))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False

arrange_by = None
icon_size = 112
text_size = 13
label_pos = "bottom"
hide_extensions = [app_name]           # "Untitled Studio", not "Untitled Studio.app"
icon_locations = {
    app_name:          (170, 190),
    "Applications":    (490, 190),
    # Only visible when Finder shows hidden files: park them outside the window.
    ".background.tiff": (330, 900),
    ".VolumeIcon.icns": (480, 900),
}
