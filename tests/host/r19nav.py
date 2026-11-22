"""r19 third pass (docs/ui/r19.txt section 3): navigation of the paused menu's
Settings, by key presses, for the suites that drive it (pad, bt, audio_ui, drive_save, ui_wired).

SETTINGS' tiles (with a game, no cheats): Volume, Brightness, Screen size, Fast forward, FPS counter, Autosave, Keys,
Controller, [Bluetooth], Link, More; the arrows go round (left and right). MORE ("More:K", three across, scrolls) is
one page behind the last tile."""
import host_tests as H

BT = H.BT


def settings_names(game=True, bt=BT, cheats=False):
    return (["volume", "brightness", "size", "fps", "autosave", "keys", "controller"] +
            (["bluetooth"] if bt else []) + (["link"] if game else []) + (["cheats"] if cheats else []) + ["more"])


def more_names(game=True, drive=False, autosave_to=False, lcd=False):
    """More (r19 fourth pass, one screenful): Frame skip, Save to, [Autosave to], [Saves on], Link device, Settings for, Keys for,
    [LCD fix], [Game: Restart], Saves: Manage, Games: Manage, About."""
    return (["frame_skip", "save_to"] + (["autosave_to"] if autosave_to else []) + (["saves_on"] if drive else []) +
            ["link_device", "settings_for", "keys_for"] + (["lcd"] if lcd else []) + (["game"] if game else []) +
            ["saves", "games", "about"])


def settings_index(name, **kw):
    return settings_names(**kw).index(name)


def more_index(name, **kw):
    return more_names(**kw).index(name)


def to_settings_row():
    """From the paused menu as it opens (SAVES, RESUME picked): up to the top row, left to SETTINGS (main:0; its tiles
    are shown, none picked)."""
    return "press up\npress left\nwait 0.3\n"


def settings_tile(name, **kw):
    """From the paused menu as it opens: into Settings' tiles (down) and along them to a tile (Settings:K)."""
    return to_settings_row() + "press down\nwait 0.2\n" + "press right\n" * settings_index(name, **kw)


def more_tile(name, **kw):
    """From the paused menu as it opens: to More's tile (More:K). (Autosave to, Saves on: pass autosave_to/drive.)"""
    game = kw.get("game", True)
    return (settings_tile("more", game=game) + "press enter\nwait 0.4\n" +
            "press right\n" * more_index(name, **kw))


def tile_moves(a, b):
    """Along a screen of tiles from tile a to tile b (right: the next one)."""
    return "press right\n" * (b - a) if b > a else "press left\n" * (a - b)
