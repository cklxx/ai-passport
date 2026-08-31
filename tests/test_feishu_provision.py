import base64
import json
from pathlib import Path
import unittest

from tools.feishu_provision import build_frame


class ProvisionFrameTest(unittest.TestCase):
    def test_frame_contains_only_expected_fields(self):
        frame = build_frame("cli_example123", "test-secret")
        prefix, encoded = frame.rstrip(b"\n").split(b" ", 1)
        self.assertEqual(prefix, b"FAP-FEISHU/1")
        self.assertEqual(
            json.loads(base64.b64decode(encoded)),
            {"app_id": "cli_example123", "app_secret": "test-secret"},
        )

    def test_rejects_non_feishu_app_id(self):
        with self.assertRaises(ValueError):
            build_frame("not-an-app", "test-secret")

    def test_rejects_empty_secret(self):
        with self.assertRaises(ValueError):
            build_frame("cli_example123", "")

    def test_usb_tool_waits_for_receiver_after_serial_reset(self):
        source = Path("tools/feishu_provision.py").read_text(encoding="utf-8")
        self.assertIn('line.endswith(f"{PROTOCOL} READY")', source)
        self.assertIn("port.write(frame)", source)
        self.assertLess(source.index("READY"), source.index("port.write(frame)"))


class MobilePortalContractTest(unittest.TestCase):
    def test_first_run_requires_owner_app_fields(self):
        source = Path("main/setup_portal.c").read_text(encoding="utf-8")
        self.assertIn("<input name=app_id required", source)
        self.assertIn("<input name=app_secret type=password required", source)

    def test_private_app_hotspot_is_password_protected(self):
        source = Path("main/setup_portal.c").read_text(encoding="utf-8")
        self.assertIn("WIFI_AUTH_WPA2_PSK", source)
        self.assertIn("random_password(info->password)", source)

    def test_invalid_client_reopens_owner_app_setup(self):
        binding = Path("main/feishu_binding.c").read_text(encoding="utf-8")
        onboarding = Path("main/product_onboarding.c").read_text(encoding="utf-8")
        self.assertIn('strcmp(oauth_error, "invalid_client") == 0', binding)
        self.assertIn("feishu_store_clear_credentials()", onboarding)
        self.assertIn("goto configure_owner_app;", onboarding)

    def test_user_message_read_scopes_are_requested(self):
        binding = Path("main/feishu_binding.c").read_text(encoding="utf-8")
        self.assertIn("im:chat:readonly", binding)
        self.assertIn("im:message.p2p_msg:get_as_user", binding)
        self.assertIn("im:message.group_msg:get_as_user", binding)


if __name__ == "__main__":
    unittest.main()
