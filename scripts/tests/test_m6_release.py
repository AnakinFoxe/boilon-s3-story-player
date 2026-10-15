"""The release name printed at boot (m6_release.h): one name in the tag format, both variants."""

import re
import unittest
from pathlib import Path

BOARD = Path(__file__).resolve().parents[2] / "main/boards/boilon-s3-gen2"
LINE = 'ESP_LOGI(kTag, "%s", boilon::release::kName);'


class ReleaseNameTests(unittest.TestCase):
    def test_one_name_whose_tag_is_the_same_text_with_a_hyphen(self):
        names = re.findall(r'kName\[\] = "([^"]*)";', (BOARD / "m6_release.h").read_text(encoding="utf-8"))
        self.assertEqual(len(names), 1)
        self.assertRegex(names[0], r"^boilon-ai-player v[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertRegex(names[0].replace(" ", "-"), r"^boilon-ai-player-v[0-9]+\.[0-9]+\.[0-9]+$")

    def test_both_variants_print_it_before_their_variant_line(self):
        for name, variant in (("m3_ai_main.cc", '"M3 AI variant;'),
                              ("m2_library_main.cc", '"Daily offline Beta;')):
            with self.subTest(name):
                source = (BOARD / name).read_text(encoding="utf-8")
                self.assertEqual(source.count('#include "m6_release.h"'), 1)
                main = source[source.index('extern "C" void app_main()'):]
                self.assertEqual(main.count(LINE), 1)
                self.assertLess(main.index(LINE), main.index(variant))


if __name__ == "__main__":
    unittest.main()
