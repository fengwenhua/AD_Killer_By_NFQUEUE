#!/usr/bin/env python3
"""Channel checks compare runtime integer values, including minified literals."""
import unittest
from test_release_channels import embedded_version_code


class VersionCodeTests(unittest.TestCase):
    def test_integer_and_equivalent_exponent_literals(self):
        for literal in ['13000', '13e3', '1.3e4', '13E+3', '130000e-1']:
            self.assertEqual(embedded_version_code('{versionCode:' + literal + '}'), 13000)
        self.assertEqual(embedded_version_code('{versionCode:13001,releaseChannel:"stable"}'), 13001)

    def test_missing_duplicate_expressions_and_out_of_range_are_rejected(self):
        for html in ['{}', '{versionCode:13000,versionCode:13000}', '{versionCode:13000+1}',
                     '{versionCode:"13000"}', '{versionCode:-1}', '{versionCode:0}',
                     '{versionCode:13000.5}', '{versionCode:13e999999}', '{versionCode:2147483648}']:
            with self.subTest(html=html), self.assertRaises(AssertionError):
                embedded_version_code(html)


if __name__ == '__main__':
    unittest.main()
