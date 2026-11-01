import json
import unittest
from pathlib import Path


PROFILE_ROOT = Path(__file__).parents[2] / "resources" / "profiles" / "Snapmaker" / "process"


class ProcessProfilesTest(unittest.TestCase):
    def test_paint_depths_do_not_exceed_inherited_shell_counts(self) -> None:
        profiles = {}
        for path in PROFILE_ROOT.glob("*.json"):
            profile = json.loads(path.read_text(encoding="utf-8-sig"))
            profiles[profile.get("name", path.stem)] = profile

        def resolve(name, ancestors=()):
            self.assertNotIn(name, ancestors, "Process profile inheritance cycle")
            self.assertIn(name, profiles, "Missing inherited process profile")
            profile = profiles[name]
            inherited = resolve(profile["inherits"], (*ancestors, name)) if profile.get("inherits") else {}
            return {**inherited, **profile}

        self.assertTrue(profiles)
        for name in profiles:
            effective = resolve(name)
            for side in ("top", "bottom"):
                paint_key = f"{side}_color_penetration_layers"
                shell_key = f"{side}_shell_layers"
                if paint_key in effective and shell_key in effective:
                    with self.subTest(profile=name, side=side):
                        self.assertLessEqual(int(effective[paint_key]), int(effective[shell_key]))


if __name__ == "__main__":
    unittest.main()
