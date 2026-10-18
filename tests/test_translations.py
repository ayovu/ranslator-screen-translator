"""Catalog validation tests; only synthetic XML in temporary directories."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "catalogs", Path(__file__).resolve().parents[1] / "scripts/update-translations.py")
catalogs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(catalogs)


class CatalogValidationTests(unittest.TestCase):
    def catalog(self, body, locale="fr"):
        directory = tempfile.TemporaryDirectory(prefix="ranslator-ts-test-")
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "ranslator_fr.ts"
        path.write_text(f'<TS version="2.1" language="{locale}" sourcelanguage="en">'
                        f'<context><name>Ranslator</name>{body}</context></TS>')
        return path

    def test_finished_translation_preserves_argument_and_cli_tokens(self):
        path = self.catalog('<message><source>Choose %1; keep {model}</source>'
                            '<translation>Conserver {model}; choisir %1</translation></message>')
        self.assertEqual(len(catalogs.messages(path)), 1)

    def test_missing_placeholder_is_rejected(self):
        path = self.catalog('<message><source>Choose %1</source>'
                            '<translation>Choisir</translation></message>')
        with self.assertRaisesRegex(ValueError, "preserve placeholders"):
            catalogs.messages(path)

    def test_each_plural_form_must_preserve_count(self):
        path = self.catalog('<message numerus="yes"><source>%n files</source><translation>'
                            '<numerusform>%n fichier</numerusform><numerusform>fichiers</numerusform>'
                            '</translation></message>')
        with self.assertRaisesRegex(ValueError, "preserve placeholders"):
            catalogs.messages(path)

    def test_unfinished_is_valid_but_empty_finished_is_not(self):
        unfinished = self.catalog('<message><source>Settings</source>'
                                  '<translation type="unfinished"/></message>')
        self.assertEqual(len(catalogs.messages(unfinished)), 1)
        finished = self.catalog('<message><source>Settings</source><translation/></message>')
        with self.assertRaisesRegex(ValueError, "empty finished"):
            catalogs.messages(finished)

    def test_filename_and_declared_locale_must_agree(self):
        with self.assertRaisesRegex(ValueError, "language must match"):
            catalogs.messages(self.catalog("", "de"))

    def test_duplicate_messages_are_rejected(self):
        message = '<message><source>Settings</source><translation type="unfinished"/></message>'
        with self.assertRaisesRegex(ValueError, "duplicate"):
            catalogs.messages(self.catalog(message + message))


if __name__ == "__main__":
    unittest.main()
