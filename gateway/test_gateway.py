import math
import unittest
import base64
import hashlib
import json
import tempfile
import threading
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

import benchmark_lanes
import companion_store
import gateway
import intelligence_router
import telemetry_analytics


def setup_sheet_vision_payload():
    image = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4"
        "z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="
    )
    page = {
        "page": 1,
        "mime_type": "image/png",
        "sha256": hashlib.sha256(image).hexdigest(),
        "data_base64": base64.b64encode(image).decode("ascii"),
    }
    return {
        "contract": "racebox-setup-sheet-vision-v1",
        "previous": {"revision_id": "setup-before", "pages": [dict(page)]},
        "current": {"revision_id": "setup-after", "pages": [dict(page)]},
    }


def official_results_photo_payload():
    image = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4"
        "z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="
    )
    return {
        "contract": "racebox-official-results-photo-v1",
        "mime_type": "image/png",
        "sha256": hashlib.sha256(image).hexdigest(),
        "data_base64": base64.b64encode(image).decode("ascii"),
    }


def synthetic_csv(
    *,
    lateral_extra: float = 0.0,
    speed_step: float = 0.25,
    braking_indicator: bool = False,
    corner_profile: str = "normal",
    latitude_offset: float = 0.0,
    track_warp: float = 0.0,
) -> str:
    lines = [
        "#contract,racebox-session-analytics-csv-v1",
        "#sample_step,1",
        "time_s,absolute_time_us,lap_raw,lap_race,lap_phase,lap_elapsed_s,lap_progress_percent,"
        "latitude,longitude,speed_kmh,"
        "lateral_g,longitudinal_g,vertical_g,yaw_rate_dps,raw_lateral_g,raw_vertical_g,steering_percent,"
        "throttle_percent,brake_percent,satellites",
    ]
    time_s = 0.0
    for lap in range(1, 4):
        for index in range(80):
            speed = 10.0 + index * speed_step
            lateral = 0.30 + speed * 0.005 + lateral_extra
            yaw = 12.0
            steering = 10.0
            throttle = 100.0
            brake = 0.0
            raw_lateral = lateral
            raw_vertical = 1.0
            if corner_profile == "clean":
                speed = 42.0
                lateral = 0.70
                raw_lateral = lateral
                steering = 35.0
                throttle = 0.0
            elif corner_profile == "overdrive":
                speed = 42.0 - max(0, index - 40) * 0.08
                lateral = 0.80 if index < 40 else 0.25
                raw_lateral = lateral
                steering = 70.0 if index < 40 or index % 2 == 0 else 88.0
                throttle = 0.0
            elif corner_profile in {"roll_flat", "roll_more"}:
                if index < 30:
                    speed = 0.0
                    lateral = 0.04
                    raw_lateral = 0.04
                    steering = 0.0
                    throttle = 0.0
                else:
                    speed = 34.0
                    lateral = 0.60
                    raw_lateral = 0.60
                    raw_vertical = 1.0 if corner_profile == "roll_flat" else 0.80
                    steering = 45.0
                    throttle = 0.0
            elif corner_profile in {"roll_rate_slow", "roll_rate_fast"}:
                if index < 30:
                    speed = 0.0
                    lateral = 0.04
                    raw_lateral = 0.04
                    steering = 0.0
                    throttle = 0.0
                else:
                    speed = 34.0
                    amplitude = 3.0 if corner_profile == "roll_rate_slow" else 10.0
                    frequency = 0.10 if corner_profile == "roll_rate_slow" else 0.25
                    angle_deg = 32.0 + amplitude * math.sin((index - 30) * frequency)
                    lateral = math.tan(math.radians(angle_deg))
                    raw_lateral = lateral
                    raw_vertical = 1.0
                    steering = 45.0
                    throttle = 0.0
            if braking_indicator and lap == 2 and 60 <= index <= 66:
                throttle = 0.0
                brake = 85.0
                yaw = 60.0
                lateral = 0.60
                raw_lateral = lateral
                braking_speeds = [30.0, 29.0, 28.0, 27.9, 27.8, 27.7, 27.6]
                speed = braking_speeds[index - 60]
            progress = index / 79 * 100.0
            latitude = (
                49.184
                + index * 0.000001
                + latitude_offset
                + track_warp * math.sin(math.radians(progress * 3.6))
            )
            longitude = -123.145 + index * 0.000001
            lines.append(
                f"{time_s:.3f},1700000000000000,{lap},{lap},complete,{index * 0.04:.3f},"
                f"{progress:.4f},{latitude:.7f},{longitude:.7f},"
                f"{speed:.4f},{lateral:.4f},0.1,0.0,{yaw:.3f},{raw_lateral:.4f},{raw_vertical:.4f},{steering:.3f},"
                f"{throttle:.3f},{brake:.3f},14"
            )
            time_s += 0.04
        time_s += 0.20
    return "\n".join(lines) + "\n"


def synthetic_whole_run_incident_csv(*, repeated_stop: bool = False) -> str:
    """Seventeen RaceBox-only laps with an optional A-main-style one-off stop."""

    lines = [
        "#contract,racebox-session-analytics-csv-v1",
        "#sample_step,1",
        "time_s,absolute_time_us,lap_raw,lap_race,lap_phase,lap_elapsed_s,lap_progress_percent,"
        "latitude,longitude,speed_kmh,"
        "lateral_g,longitudinal_g,vertical_g,yaw_rate_dps,raw_lateral_g,raw_vertical_g,steering_percent,"
        "throttle_percent,brake_percent,satellites",
    ]
    time_s = 0.0
    samples = 403
    event_index = 379
    event_speeds = [8.0, 3.77, 4.4, 5.2, 6.1, 7.4, 9.0]
    for race_lap, raw_lap in enumerate(range(2, 19), start=1):
        lap_duration = 16.08
        if raw_lap == 8:
            lap_duration = 16.84
        elif raw_lap > 8:
            lap_duration = 16.756
        sample_period = lap_duration / (samples - 1)
        for index in range(samples):
            progress = index / (samples - 1) * 100.0
            lap_elapsed = index * sample_period
            speed = 34.0 + 8.0 * math.sin(math.radians(progress * 7.2))
            if index == event_index - 1:
                speed = 24.59
            event_applies = repeated_stop or raw_lap == 8
            if event_applies and event_index <= index < event_index + len(event_speeds):
                speed = event_speeds[index - event_index]
            lateral = 0.45 * math.sin(math.radians(progress * 10.8))
            longitudinal = -0.10 if event_applies and event_index <= index <= event_index + 2 else 0.08
            yaw = 25.0 * math.sin(math.radians(progress * 7.2))
            latitude = 49.184 + 0.00008 * math.sin(math.radians(progress * 3.6))
            longitude = -123.145 + 0.00008 * math.cos(math.radians(progress * 3.6))
            absolute_time_us = 1_722_643_200_000_000 + int(time_s * 1_000_000)
            lines.append(
                f"{time_s:.6f},{absolute_time_us},{raw_lap},{race_lap},complete,{lap_elapsed:.6f},"
                f"{progress:.4f},{latitude:.7f},{longitude:.7f},{speed:.4f},"
                f"{lateral:.4f},{longitudinal:.4f},0.0,{yaw:.3f},{lateral:.4f},1.0,,"
                f",,14"
            )
            time_s += sample_period
        time_s += 0.20
    return "\n".join(lines) + "\n"


def synthetic_line_and_slowing_csv(
    *, line_change_m: float = 0.65, lap_count: int = 5, late_fast_lap: bool = False
) -> str:
    """Five RaceBox-only laps with four corners and a distinct fastest-lap line."""

    lines = [
        "#contract,racebox-session-analytics-csv-v1",
        "#sample_step,1",
        "time_s,absolute_time_us,lap_raw,lap_race,lap_phase,lap_elapsed_s,lap_progress_percent,"
        "latitude,longitude,speed_kmh,"
        "lateral_g,longitudinal_g,vertical_g,yaw_rate_dps,raw_lateral_g,raw_vertical_g,steering_percent,"
        "throttle_percent,brake_percent,satellites",
    ]
    time_s = 0.0
    samples = 201
    corner_centres = (12.0, 37.0, 62.0, 87.0)
    metres_per_degree_lat = 111_320.0
    metres_per_degree_lon = metres_per_degree_lat * math.cos(math.radians(49.184))
    for lap in range(1, lap_count + 1):
        duration = (
            15.55
            if late_fast_lap and lap == lap_count
            else 15.80
            if lap == 3
            else 16.35 + abs(lap - 3) * 0.05
        )
        sample_period = duration / (samples - 1)
        for index in range(samples):
            progress = index / (samples - 1) * 100.0
            angle = math.radians(progress * 3.6)
            radius_m = 10.0
            if lap == 3:
                distance_from_line_change = abs(progress - 37.0)
                if distance_from_line_change <= 5.0:
                    radius_m += line_change_m * (1.0 - distance_from_line_change / 5.0)
            # Small whole-lap offsets exercise translation correction without
            # hiding the local fastest-lap line difference.
            east_m = radius_m * math.cos(angle) + (lap - 3) * 0.08
            north_m = radius_m * math.sin(angle) - (lap - 3) * 0.05
            latitude = 49.184 + north_m / metres_per_degree_lat
            longitude = -123.145 + east_m / metres_per_degree_lon

            nearest_corner = min(
                corner_centres,
                key=lambda centre: abs(progress - centre),
            )
            corner_distance = abs(progress - nearest_corner)
            lateral = 0.03
            yaw = 3.0
            if corner_distance <= 4.0:
                sign = 1.0 if corner_centres.index(nearest_corner) % 2 == 0 else -1.0
                lateral = sign * (0.58 - corner_distance * 0.06)
                yaw = sign * (38.0 - corner_distance * 4.0)

            speed = 45.0
            onset_distance = 10.0 if lap == 3 and nearest_corner == 37.0 else 7.0
            if -onset_distance <= progress - nearest_corner <= 0.0:
                fraction = (progress - (nearest_corner - onset_distance)) / onset_distance
                minimum = 33.0 if lap == 3 and nearest_corner == 37.0 else 30.0
                speed = 45.0 - (45.0 - minimum) * fraction
            elif 0.0 < progress - nearest_corner <= 7.0:
                minimum = 33.0 if lap == 3 and nearest_corner == 37.0 else 30.0
                fraction = (progress - nearest_corner) / 7.0
                speed = minimum + (45.0 - minimum) * fraction

            absolute_time_us = 1_722_643_200_000_000 + int(time_s * 1_000_000)
            lines.append(
                f"{time_s:.6f},{absolute_time_us},{lap},{lap},complete,{index * sample_period:.6f},"
                f"{progress:.4f},{latitude:.8f},{longitude:.8f},{speed:.4f},"
                f"{lateral:.4f},0.0000,1.0000,{yaw:.3f},{lateral:.4f},1.0000,,, ,14".replace(
                    ", ,", ",,"
                )
            )
            time_s += sample_period
        time_s += 0.20
    return "\n".join(lines) + "\n"


class CompanionStoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.now = 1_700_000_000
        root = Path(self.temporary.name)
        self.store = companion_store.CompanionStore(
            root / "companion.sqlite3",
            root / "backups",
            clock=lambda: self.now,
        )

    def tearDown(self):
        self.temporary.cleanup()

    def create_session(self, history=None):
        return self.store.create_session(
            title="Richmond R6",
            context={"run": {"label": "R6"}},
            review={"contract": "racebox-session-review-v1", "quality": {"confidence": 82}},
            prior_results=[],
            history=history or [],
            public_gateway="http://100.64.10.20:18804",
        )

    def test_pairing_is_one_use_scoped_and_revocable(self):
        created = self.create_session()
        paired = self.store.pair_device(created["pairing_code"], "Alex S25 Ultra")
        principal = self.store.authenticate(created["session_id"], paired["session_token"])
        self.assertEqual("device", principal["kind"])
        self.assertEqual("Alex S25 Ultra", principal["device_name"])
        with self.assertRaisesRegex(ValueError, "already used"):
            self.store.pair_device(created["pairing_code"], "Second phone")
        self.assertTrue(self.store.revoke_pairing(paired["pairing_id"], created["session_id"]))
        self.assertIsNone(self.store.authenticate(created["session_id"], paired["session_token"]))

    def test_pairing_expires_after_five_minutes(self):
        created = self.create_session()
        self.now += companion_store.PAIRING_LIFETIME_SECONDS + 1
        with self.assertRaisesRegex(ValueError, "expired"):
            self.store.pair_device(created["pairing_code"], "Alex S25 Ultra")

    def test_jobs_are_idempotent_and_survive_store_restart(self):
        created = self.create_session([{"role": "user", "content": "Earlier question"}])
        first = self.store.create_message_job(
            created["session_id"],
            message_id="phone-message-1",
            content="Did the car fade?",
            origin="Alex S25 Ultra",
        )
        duplicate = self.store.create_message_job(
            created["session_id"],
            message_id="phone-message-1",
            content="Did the car fade?",
            origin="Alex S25 Ultra",
        )
        self.assertFalse(first["duplicate"])
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(first["job_id"], duplicate["job_id"])
        active = self.store.begin_job(first["job_id"])
        self.assertEqual("Did the car fade?", active["question"])
        self.store.finish_job(
            first["job_id"],
            {"ok": True, "report": {"summary": "Pace stayed consistent."}},
        )
        restarted = companion_store.CompanionStore(
            self.store.database_path,
            self.store.backup_directory,
            clock=lambda: self.now,
        )
        self.assertEqual("completed", restarted.get_job(first["job_id"])["status"])
        messages = restarted.list_messages(created["session_id"])
        self.assertEqual(3, messages["total"])
        self.assertEqual("Pace stayed consistent.", messages["messages"][-1]["content"])

    def test_cancelled_job_discards_a_late_reply_and_survives_failure(self):
        created = self.create_session()
        queued = self.store.create_message_job(
            created["session_id"],
            message_id="phone-stop-1",
            content="Give me a long review.",
            origin="Alex S25 Ultra",
        )
        active = self.store.begin_job(queued["job_id"])
        self.assertEqual("thinking", active["status"])
        cancelled = self.store.cancel_job(queued["job_id"])
        self.assertTrue(cancelled["cancelled"])
        self.assertEqual("cancelled", cancelled["status"])
        self.assertIsNone(
            self.store.finish_job(
                queued["job_id"],
                {"ok": True, "report": {"summary": "This reply arrived too late."}},
            )
        )
        self.store.fail_job(queued["job_id"], "late upstream failure")
        self.assertEqual("cancelled", self.store.get_job(queued["job_id"])["status"])
        messages = self.store.list_messages(created["session_id"])
        self.assertEqual(1, messages["total"])
        self.assertEqual("Give me a long review.", messages["messages"][0]["content"])

    def test_companion_message_keeps_evidence_uncertainty_and_next_test_concise(self):
        content = companion_store._report_message_content(
            {
                "report": {
                    "summary": "The fastest lap gained most of its time in corner two.",
                    "observations": [
                        {
                            "area": "Corner two",
                            "change": "0.18 s quicker than the other laps",
                            "meaning": "Speed began falling farther before the apex and minimum speed stayed higher.",
                        }
                    ],
                    "confounds": [
                        "The GPS line shift is associated with the gain but does not prove it caused the gain."
                    ],
                    "next_test": "Repeat that approach for three clean laps.",
                    "causality_note": "This run shows where the lap differed, not why the difference occurred.",
                }
            }
        )
        self.assertIn("fastest lap gained", content)
        self.assertIn("Key evidence:", content)
        self.assertIn("Corner two", content)
        self.assertIn("Next test:", content)
        self.assertIn("Main uncertainty:", content)
        self.assertNotIn("What this proves:", content)
        self.assertNotIn("Speed began falling farther", content)
        self.assertLessEqual(len(content), 1_200)

    def test_companion_message_omits_empty_next_test_when_not_useful(self):
        content = companion_store._report_message_content(
            {
                "report": {
                    "summary": "Stopping for a quick marshal repair left a realistic chance to retain fourth, but the measured gap makes third uncertain.",
                    "observations": [],
                    "confounds": ["The exact repair duration was not recorded."],
                    "next_test": "",
                    "causality_note": "This is a timing estimate from the completed race, not a repeatable experiment.",
                }
            }
        )
        self.assertNotIn("Next test:", content)

    def test_clear_creates_complete_backup_before_deleting_messages(self):
        created = self.create_session([{"role": "user", "content": "Keep this test"}])
        result = self.store.backup_and_clear(created["session_id"])
        backup = self.store.backup_directory / result["backup_name"]
        self.assertTrue(backup.is_file())
        payload = json.loads(backup.read_text(encoding="utf-8"))
        self.assertEqual("Keep this test", payload["messages"][0]["content"])
        self.assertEqual(0, self.store.list_messages(created["session_id"])["total"])

    def test_database_never_contains_analytics_csv(self):
        created = self.create_session()
        database_bytes = self.store.database_path.read_bytes()
        self.assertNotIn(b"analytics_csv", database_bytes)
        self.assertEqual(
            "racebox-session-review-v1",
            self.store.get_session(created["session_id"])["review"]["contract"],
        )


class CompanionHttpTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.previous_store = gateway.COMPANION_STORE
        self.previous_client_token = gateway.CLIENT_TOKEN
        gateway.COMPANION_STORE = companion_store.CompanionStore(
            root / "companion.sqlite3", root / "backups"
        )
        gateway.CLIENT_TOKEN = ""
        self.server = gateway.ThreadingHTTPServer(("127.0.0.1", 0), gateway.CrewChiefHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        gateway.COMPANION_STORE = self.previous_store
        gateway.CLIENT_TOKEN = self.previous_client_token
        self.temporary.cleanup()

    def request(self, method, path, body=None, token=""):
        data = json.dumps(body).encode("utf-8") if body is not None else None
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        request = urllib.request.Request(
            self.base + path, data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_health_advertises_v14_prompt_single_sol_control_review_vision_and_requires_configured_token(self):
        status, health = self.request("GET", "/health")
        self.assertEqual(200, status)
        self.assertEqual("racebox-crew-chief-v14", health["contract"])
        self.assertEqual("racebox-crew-chief-connection-v1", health["connection_contract"])
        self.assertEqual(
            "racebox-crew-chief-behavior-v14-public-1", health["prompt_revision"]
        )
        self.assertEqual("racebox-single-agent-v1", health["pipeline"])
        self.assertEqual("single_agent", health["route"])
        self.assertIsNone(health["brain"])
        self.assertEqual(
            "openai/gpt-5.6-sol", health["primary_worker"]["model"]
        )
        self.assertEqual("xhigh", health["primary_worker"]["thinking"])
        self.assertFalse(health["ultra_enabled"])
        self.assertFalse(health["legacy_adaptive"]["active"])
        self.assertEqual(
            "racebox-companion-job-control-v1",
            health["companion_job_control_contract"],
        )
        self.assertIn("cancelled", health["companion_job_statuses"])
        self.assertEqual(
            "racebox-single-agent-test-v1",
            health["single_agent_test"]["pipeline"],
        )
        self.assertEqual(
            "/v1/crew-chief/chat/single-agent-test",
            health["single_agent_test"]["path"],
        )
        self.assertEqual(
            "openai/gpt-5.6-sol", health["single_agent_test"]["model"]
        )
        self.assertEqual("xhigh", health["single_agent_test"]["thinking"])
        self.assertEqual(
            "racebox-session-review-v5", health["session_review_contract"]
        )
        self.assertEqual(
            "racebox-session-chat-request-v1",
            health["session_chat_request_contract"],
        )
        self.assertEqual(
            "racebox-race-day-request-v4", health["race_day_request_contract"]
        )
        self.assertEqual(
            "racebox-setup-sheet-vision-v1",
            health["setup_sheet_vision_contract"],
        )
        self.assertFalse(health["setup_sheet_vision_retained"])
        self.assertEqual(
            "racebox-official-results-photo-v1",
            health["official_results_photo_contract"],
        )
        self.assertFalse(health["official_results_photo_retained"])
        self.assertEqual(
            "openai/gpt-5.6-sol", health["vision_worker"]["model"]
        )
        self.assertEqual("xhigh", health["vision_worker"]["thinking"])

        gateway.CLIENT_TOKEN = "configured-for-test"
        status, unauthorized = self.request(
            "POST",
            "/v1/crew-chief/chat",
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "Review the run.",
                "session": {"context": {}, "analytics_csv": synthetic_csv()},
            },
        )
        self.assertEqual(401, status)
        self.assertEqual("unauthorized", unauthorized["error"])
        status, unauthorized = self.request(
            "POST",
            gateway.SINGLE_AGENT_TEST_ROUTE,
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "Review the run.",
                "session": {"context": {}, "analytics_csv": synthetic_csv()},
            },
        )
        self.assertEqual(401, status)
        self.assertEqual("unauthorized", unauthorized["error"])
        gateway.CLIENT_TOKEN = ""

    def test_single_agent_http_test_route_uses_direct_pipeline(self):
        response = {
            "ok": True,
            "pipeline": gateway.SINGLE_AGENT_TEST_PIPELINE,
            "report": {"summary": "Direct test answer."},
        }
        with mock.patch("gateway.call_openclaw", return_value=response) as call:
            status, body = self.request(
                "POST",
                gateway.SINGLE_AGENT_TEST_ROUTE,
                {
                    "contract": "racebox-session-chat-request-v1",
                    "question": "What did I do best?",
                    "session": {"context": {}, "analytics_csv": synthetic_csv()},
                },
            )
        self.assertEqual(200, status)
        self.assertEqual(gateway.SINGLE_AGENT_TEST_PIPELINE, body["pipeline"])
        self.assertEqual("single_agent_test", call.call_args.kwargs["pipeline_mode"])

    def test_normal_chat_route_uses_primary_single_agent_pipeline(self):
        response = {
            "ok": True,
            "pipeline": gateway.PIPELINE_NAME,
            "report": {"summary": "Primary direct answer."},
        }
        with mock.patch("gateway.call_openclaw", return_value=response) as call:
            status, body = self.request(
                "POST",
                "/v1/crew-chief/chat",
                {
                    "contract": "racebox-session-chat-request-v1",
                    "question": "Review the run.",
                    "session": {"context": {}, "analytics_csv": synthetic_csv()},
                },
            )
        self.assertEqual(200, status)
        self.assertEqual(gateway.PIPELINE_NAME, body["pipeline"])
        self.assertEqual("single_agent", call.call_args.kwargs["pipeline_mode"])

    def test_create_pair_sync_idempotency_authorization_and_backup_clear(self):
        status, created = self.request(
            "POST",
            "/v1/crew-chief/companion/sessions",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "title": "Richmond R6",
                "session": {
                    "context": {"run": {"label": "R6"}},
                    "analytics_csv": synthetic_csv(),
                },
                "history": [{"role": "user", "content": "Keep this history"}],
            },
        )
        self.assertEqual(201, status)
        self.assertFalse(created["raw_csv_stored"])
        status, paired = self.request(
            "POST",
            "/v1/crew-chief/companion/pair",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "code": created["pairing_code"],
                "device_name": "Alex S25 Ultra",
            },
        )
        self.assertEqual(200, status)
        session_id = created["session_id"]
        token = paired["session_token"]
        status, unauthorized = self.request(
            "GET", f"/v1/crew-chief/companion/sessions/{session_id}/messages"
        )
        self.assertEqual(401, status)
        self.assertEqual("unauthorized", unauthorized["error"])
        with mock.patch.object(gateway, "process_companion_job", return_value=None):
            phone_body = {
                "contract": companion_store.COMPANION_CONTRACT,
                "message_id": "android-1",
                "text": "Did the car fade?",
            }
            viewer_body = {
                "contract": companion_store.COMPANION_CONTRACT,
                "message_id": "viewer-1",
                "text": "What should I test next?",
            }
            with ThreadPoolExecutor(max_workers=2) as executor:
                phone_request = executor.submit(
                    self.request,
                    "POST",
                    f"/v1/crew-chief/companion/sessions/{session_id}/messages",
                    phone_body,
                    token,
                )
                viewer_request = executor.submit(
                    self.request,
                    "POST",
                    f"/v1/crew-chief/companion/sessions/{session_id}/messages",
                    viewer_body,
                    created["viewer_token"],
                )
                first_status, first = phone_request.result()
                viewer_status, _ = viewer_request.result()
            duplicate_status, duplicate = self.request(
                "POST",
                f"/v1/crew-chief/companion/sessions/{session_id}/messages",
                phone_body,
                token,
            )
        self.assertEqual(202, first_status)
        self.assertEqual(202, viewer_status)
        self.assertEqual(200, duplicate_status)
        self.assertEqual(first["job_id"], duplicate["job_id"])
        status, messages = self.request(
            "GET",
            f"/v1/crew-chief/companion/sessions/{session_id}/messages?after=0",
            token=token,
        )
        self.assertEqual(200, status)
        self.assertEqual(3, messages["total"])
        origins = {item["id"]: item["origin"] for item in messages["messages"]}
        self.assertEqual("phone", origins["android-1"])
        self.assertEqual("viewer", origins["viewer-1"])
        status, cleared = self.request(
            "DELETE",
            f"/v1/crew-chief/companion/sessions/{session_id}/messages",
            token=token,
        )
        self.assertEqual(200, status)
        self.assertTrue(cleared["backup_created"])
        self.assertTrue((gateway.COMPANION_STORE.backup_directory / cleared["backup_name"]).is_file())

    def test_companion_job_cancel_is_authorized_idempotent_and_keeps_question(self):
        status, created = self.request(
            "POST",
            "/v1/crew-chief/companion/sessions",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "title": "Q3",
                "session": {
                    "context": {"run": {"label": "Q3"}},
                    "analytics_csv": synthetic_csv(),
                },
            },
        )
        self.assertEqual(201, status)
        status, paired = self.request(
            "POST",
            "/v1/crew-chief/companion/pair",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "code": created["pairing_code"],
                "device_name": "Alex S25 Ultra",
            },
        )
        self.assertEqual(200, status)
        message_path = (
            f"/v1/crew-chief/companion/sessions/{created['session_id']}/messages"
        )
        with mock.patch.object(gateway, "process_companion_job", return_value=None):
            status, queued = self.request(
                "POST",
                message_path,
                {
                    "contract": companion_store.COMPANION_CONTRACT,
                    "message_id": "phone-cancel-http-1",
                    "text": "Review every corner.",
                },
                paired["session_token"],
            )
        self.assertEqual(202, status)
        cancel_path = (
            f"/v1/crew-chief/companion/jobs/{queued['job_id']}/cancel"
        )
        status, unauthorized = self.request(
            "POST",
            cancel_path,
            {"contract": gateway.COMPANION_JOB_CONTROL_CONTRACT},
        )
        self.assertEqual(401, status)
        self.assertEqual("unauthorized", unauthorized["error"])
        status, cancelled = self.request(
            "POST",
            cancel_path,
            {"contract": gateway.COMPANION_JOB_CONTROL_CONTRACT},
            paired["session_token"],
        )
        self.assertEqual(200, status)
        self.assertTrue(cancelled["cancelled"])
        self.assertEqual("cancelled", cancelled["status"])
        status, repeated = self.request(
            "POST",
            cancel_path,
            {"contract": gateway.COMPANION_JOB_CONTROL_CONTRACT},
            paired["session_token"],
        )
        self.assertEqual(200, status)
        self.assertTrue(repeated["cancelled"])
        status, job = self.request(
            "GET",
            f"/v1/crew-chief/companion/jobs/{queued['job_id']}",
            token=paired["session_token"],
        )
        self.assertEqual(200, status)
        self.assertEqual("cancelled", job["status"])
        messages = gateway.COMPANION_STORE.list_messages(created["session_id"])
        self.assertEqual(1, messages["total"])
        self.assertEqual("Review every corner.", messages["messages"][0]["content"])

    def test_companion_photo_fails_closed_then_queues_without_storing_image(self):
        status, created = self.request(
            "POST",
            "/v1/crew-chief/companion/sessions",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "title": "Q2",
                "session": {
                    "context": {"run": {"label": "Q2"}},
                    "analytics_csv": synthetic_csv(),
                },
            },
        )
        self.assertEqual(201, status)
        status, paired = self.request(
            "POST",
            "/v1/crew-chief/companion/pair",
            {
                "contract": companion_store.COMPANION_CONTRACT,
                "code": created["pairing_code"],
                "device_name": "Alex S25 Ultra",
            },
        )
        self.assertEqual(200, status)
        path = (
            f"/v1/crew-chief/companion/sessions/{created['session_id']}/messages"
        )
        body = {
            "contract": companion_store.COMPANION_CONTRACT,
            "message_id": "official-result-photo-1",
            "text": "Read this official Q2 result.",
            "attachment": official_results_photo_payload(),
        }

        with mock.patch.object(gateway, "VISION_ENABLED", False):
            status, unavailable = self.request(
                "POST", path, body, paired["session_token"]
            )
        self.assertEqual(400, status)
        self.assertIn("unavailable", unavailable["error"])
        self.assertEqual(
            0,
            gateway.COMPANION_STORE.list_messages(created["session_id"])["total"],
        )

        with mock.patch.object(gateway, "VISION_ENABLED", True), mock.patch.object(
            gateway, "process_companion_job", return_value=None
        ):
            status, queued = self.request(
                "POST", path, body, paired["session_token"]
            )
        self.assertEqual(202, status)
        self.assertEqual("queued", queued["status"])
        self.assertEqual(
            1,
            gateway.COMPANION_STORE.list_messages(created["session_id"])["total"],
        )
        database_bytes = gateway.COMPANION_STORE.database_path.read_bytes()
        self.assertNotIn(
            body["attachment"]["data_base64"].encode("ascii"), database_bytes
        )


class GatewayContractTests(unittest.TestCase):
    def test_system_prompt_requires_driver_facing_wording(self):
        self.assertIn("pit-lane helper", gateway.SYSTEM_PROMPT)
        self.assertIn("racing meaning", gateway.SYSTEM_PROMPT)
        self.assertIn("Do not use coding terms", gateway.SYSTEM_PROMPT)
        self.assertIn("push/understeer", gateway.SYSTEM_PROMPT)
        self.assertIn("Overdriving evidence", gateway.SYSTEM_PROMPT)
        self.assertIn("Roll-rate evidence", gateway.SYSTEM_PROMPT)
        self.assertIn("every lap", gateway.SYSTEM_PROMPT)
        self.assertIn("needs_driver_context", gateway.SYSTEM_PROMPT)
        self.assertIn("Missing Sanwa is not a reason to reject", gateway.SYSTEM_PROMPT)
        self.assertIn("you began turning left", gateway.SYSTEM_PROMPT)
        self.assertIn("what did I do best?", gateway.SYSTEM_PROMPT)
        self.assertIn("likely driver-action inferences", gateway.SYSTEM_PROMPT)
        self.assertIn("Do not retreat to \"cannot tell\"", gateway.SYSTEM_PROMPT)
        self.assertIn("never substitute all laps", gateway.SYSTEM_PROMPT)
        self.assertIn("coherent full-corner path", gateway.SYSTEM_PROMPT)
        self.assertIn("Do not append a next-lap experiment", gateway.SYSTEM_PROMPT)
        self.assertIn("official-results photo is a transcription source", gateway.SYSTEM_PROMPT)
        self.assertIn("Keep printed", gateway.SYSTEM_PROMPT)
        self.assertIn("race results separate", gateway.SYSTEM_PROMPT)
        self.assertIn("actually trying to learn or decide", gateway.SYSTEM_PROMPT)
        self.assertIn("transferable decision guidance", gateway.SYSTEM_PROMPT)
        self.assertIn("Guidance for next time is not automatically", gateway.SYSTEM_PROMPT)
        self.assertIn("otherwise an empty string", gateway.SYSTEM_PROMPT)
        self.assertIn("keep the complete visible answer under 120 words", gateway.SYSTEM_PROMPT)
        self.assertIn("Resolve the exact subject and comparison", gateway.SYSTEM_PROMPT)
        self.assertIn("Earlier Crew Chief replies", gateway.SYSTEM_PROMPT)
        self.assertIn("use both sources", gateway.SYSTEM_PROMPT)

    def test_request_requires_versioned_evidence(self):
        with self.assertRaisesRegex(ValueError, "unsupported evidence contract"):
            gateway.validate_request(
                {
                    "setup_change": "rear spring 2.6 to 2.8",
                    "question": "What changed?",
                    "evidence": {"contract": "wrong"},
                }
            )

    def test_request_bounds_history(self):
        value = gateway.validate_request(
            {
                "setup_change": "rear spring 2.6 to 2.8",
                "question": "What changed?",
                "evidence": {"contract": "racebox-crew-chief-evidence-v1"},
                "history": [
                    {"role": "user", "content": f"question {index}"}
                    for index in range(20)
                ],
            }
        )
        self.assertEqual(10, len(value["history"]))
        self.assertEqual("question 10", value["history"][0]["content"])

    def test_lap_chat_can_receive_bounded_setup_knowledge(self):
        value = gateway.validate_request(
            {
                "setup_change": "rear spring 2.6 to 2.8",
                "question": "Why is the rear nervous on power?",
                "evidence": {"contract": "racebox-crew-chief-evidence-v1"},
                "prior_setup_results": [
                    {
                        "record_id": "setup-result-spring",
                        "track_name": "RC Raceway",
                        "setup_change": "rear spring 2.6 to 2.8",
                        "driver_result": "nervous on power",
                        "confidence": 78,
                    }
                ],
            }
        )
        self.assertEqual(
            "setup-result-spring", value["prior_setup_results"][0]["record_id"]
        )
        payload, analytics = gateway._prepare_evidence(value)
        self.assertIsNone(analytics)
        self.assertEqual(
            "setup-result-spring",
            payload["relevant_prior_setup_results"][0]["record_id"],
        )

    def test_report_is_normalized_and_bounded(self):
        report = gateway.normalize_report(
            {
                "verdict": "impossible",
                "confidence": 900,
                "summary": "Measured association only.",
                "observations": [
                    {
                        "area": "Steering",
                        "change": "P95 input fell.",
                        "meaning": "Less peak input was recorded.",
                        "evidence_ids": ["channel:steering:p95"],
                    }
                ]
                * 12,
                "confounds": ["Driver line changed."] * 12,
            }
        )
        self.assertEqual("inconclusive", report["verdict"])
        self.assertEqual(100, report["confidence"])
        self.assertEqual(8, len(report["observations"]))
        self.assertEqual(8, len(report["confounds"]))

    def test_json_extractor_tolerates_fences_without_trusting_them(self):
        parsed = gateway.parse_json_object('prefix {"verdict":"mixed"} suffix')
        self.assertEqual("mixed", parsed["verdict"])

    def test_race_day_request_requires_both_runs(self):
        with self.assertRaisesRegex(ValueError, "current run is required"):
            gateway.validate_race_day_request(
                {
                    "contract": "racebox-race-day-request-v1",
                    "question": "What changed?",
                    "previous": {
                        "context": {},
                        "analytics_csv": synthetic_csv(),
                    },
                }
            )

    def test_race_day_v3_bounds_prior_setup_results(self):
        supplied_results = [
            {
                "record_id": f"setup-{index}",
                "track_name": "RC Raceway",
                "setup_change": "rear spring changed " + ("x" * 3000),
                "driver_result": "more rotation",
                "confidence": 78,
                "observations": [
                    {
                        "area": "Lateral response",
                        "change": "+0.03 G",
                        "meaning": "More response was measured.",
                        "evidence_ids": ["dynamics:lateral:matched-speed-steering"],
                    }
                ]
                * 10,
                "evidence_summary": {
                    "analytics_contract": "racebox-setup-analytics-v3",
                    "formula_version": 3,
                    "quality_confidence": 84,
                },
            }
            for index in range(12)
        ]
        value = gateway.validate_race_day_request(
            {
                "contract": "racebox-race-day-request-v3",
                "question": "Did the spring change help rear grip?",
                "previous": {"context": {}, "analytics_csv": synthetic_csv()},
                "current": {"context": {}, "analytics_csv": synthetic_csv()},
                "prior_setup_results": supplied_results,
            }
        )
        self.assertEqual(8, len(value["prior_setup_results"]))
        self.assertEqual(2000, len(value["prior_setup_results"][0]["setup_change"]))
        self.assertEqual(5, len(value["prior_setup_results"][0]["observations"]))
        payload, analytics = gateway._prepare_evidence(value)
        self.assertIsNotNone(analytics)
        self.assertEqual(
            "setup-0", payload["relevant_prior_setup_results"][0]["record_id"]
        )
        self.assertNotIn("analytics_csv", str(payload))

    def test_race_day_v4_validates_bounded_setup_sheet_vision(self):
        value = gateway.validate_race_day_request(
            {
                "contract": "racebox-race-day-request-v4",
                "question": "What setup change is visible and supported?",
                "previous": {"context": {}, "analytics_csv": synthetic_csv()},
                "current": {"context": {}, "analytics_csv": synthetic_csv()},
                "setup_sheet_vision": setup_sheet_vision_payload(),
            }
        )
        self.assertEqual(2, value["setup_sheet_vision"]["image_count"])
        self.assertGreater(value["setup_sheet_vision"]["decoded_bytes"], 0)
        payload, _ = gateway._prepare_evidence(value)
        self.assertTrue(payload["setup_sheet_visual_evidence"]["requested"])
        self.assertNotIn("data_base64", str(payload))

    def test_setup_sheet_vision_rejects_paths_hashes_types_and_oversize(self):
        base = setup_sheet_vision_payload()
        bad_path = json.loads(json.dumps(base))
        bad_path["current"]["pages"][0]["local_path"] = "C:/private/setup.pdf"
        with self.assertRaisesRegex(ValueError, "unsupported fields"):
            gateway.validate_setup_sheet_vision(bad_path)

        bad_hash = json.loads(json.dumps(base))
        bad_hash["current"]["pages"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "does not match"):
            gateway.validate_setup_sheet_vision(bad_hash)

        bad_type = json.loads(json.dumps(base))
        bad_type["current"]["pages"][0]["mime_type"] = "application/pdf"
        with self.assertRaisesRegex(ValueError, "type is unsupported"):
            gateway.validate_setup_sheet_vision(bad_type)

        oversized = b"x" * (gateway.MAX_VISION_IMAGE_BYTES + 1)
        too_large = json.loads(json.dumps(base))
        too_large["current"]["pages"][0].update(
            {
                "sha256": hashlib.sha256(oversized).hexdigest(),
                "data_base64": base64.b64encode(oversized).decode("ascii"),
            }
        )
        with self.assertRaisesRegex(ValueError, "too large"):
            gateway.validate_setup_sheet_vision(too_large)

    def test_setup_sheet_vision_is_sent_only_when_explicit_and_proven(self):
        request = gateway.validate_race_day_request(
            {
                "contract": "racebox-race-day-request-v4",
                "question": "Compare the setup sheets.",
                "previous": {"context": {}, "analytics_csv": synthetic_csv()},
                "current": {"context": {}, "analytics_csv": synthetic_csv()},
                "setup_sheet_vision": setup_sheet_vision_payload(),
            }
        )
        worker_contents = []

        def fake_completion(route, messages, timeout):
            if route == intelligence_router.ROUTER_SPEC["route"]:
                return {
                    "task_type": "setup comparison",
                    "lane": "analysis",
                    "confidence": 90,
                    "reasons": ["Two setup sheets are supplied."],
                }
            worker_contents.append(messages[-1]["content"])
            return {
                "verdict": "inconclusive",
                "confidence": 70,
                "summary": "The structured values are available for comparison.",
                "observations": [],
                "confounds": [],
                "next_test": "Reconcile the sheet with the car.",
                "causality_note": "The sheet does not prove a performance cause.",
            }

        with mock.patch.object(gateway, "VISION_ENABLED", False), mock.patch(
            "gateway.openclaw_completion", side_effect=fake_completion
        ):
            disabled = gateway.call_openclaw(request)
        self.assertIsInstance(worker_contents[-1], str)
        self.assertFalse(disabled["setup_sheet_vision"]["used"])
        self.assertFalse(disabled["setup_sheet_vision"]["available"])

        with mock.patch.object(gateway, "VISION_ENABLED", True), mock.patch(
            "gateway.openclaw_completion", side_effect=fake_completion
        ):
            enabled = gateway.call_openclaw(request)
        self.assertIsInstance(worker_contents[-1], list)
        image_parts = [
            item for item in worker_contents[-1] if item.get("type") == "image_url"
        ]
        self.assertEqual(2, len(image_parts))
        self.assertTrue(enabled["setup_sheet_vision"]["used"])
        self.assertFalse(enabled["setup_sheet_vision"]["retained"])

    def test_official_results_photo_validates_hash_type_magic_and_size(self):
        valid = gateway.validate_official_results_photo(
            official_results_photo_payload()
        )
        self.assertEqual("racebox-official-results-photo-v1", valid["contract"])
        self.assertGreater(valid["decoded_bytes"], 0)

        bad_path = official_results_photo_payload()
        bad_path["local_path"] = "C:/private/results.jpg"
        with self.assertRaisesRegex(ValueError, "unsupported fields"):
            gateway.validate_official_results_photo(bad_path)

        bad_hash = official_results_photo_payload()
        bad_hash["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "does not match"):
            gateway.validate_official_results_photo(bad_hash)

        bad_magic = official_results_photo_payload()
        bad_magic["mime_type"] = "image/jpeg"
        with self.assertRaisesRegex(ValueError, "not a JPEG"):
            gateway.validate_official_results_photo(bad_magic)

        oversized = b"x" * (gateway.MAX_VISION_IMAGE_BYTES + 1)
        too_large = official_results_photo_payload()
        too_large.update(
            {
                "sha256": hashlib.sha256(oversized).hexdigest(),
                "data_base64": base64.b64encode(oversized).decode("ascii"),
            }
        )
        with self.assertRaisesRegex(ValueError, "too large"):
            gateway.validate_official_results_photo(too_large)

    def test_official_results_photo_uses_proven_sol_lane_without_brain_or_retention(self):
        photo = gateway.validate_official_results_photo(
            official_results_photo_payload()
        )
        request = {
            "mode": "official_results_photo",
            "contract": companion_store.COMPANION_CONTRACT,
            "question": "Read this official result and add it to Q2.",
            "session": {
                "context": {"run": {"label": "Q2"}},
                "review": {"contract": "racebox-session-review-v4"},
            },
            "prior_setup_results": [],
            "history": [],
            "official_results_photo": photo,
        }
        calls = []

        def fake_completion(route, messages, timeout):
            calls.append((route, messages))
            return {
                "verdict": "supported",
                "confidence": 92,
                "summary": "The sheet clearly lists Alex Pate in second place for Q2.",
                "observations": [
                    {
                        "area": "Official Q2 result",
                        "change": "Position 2, 19 laps",
                        "meaning": "These are printed race-result facts, not telemetry conclusions.",
                        "evidence_ids": ["official-results-photo:test"],
                    }
                ],
                "confounds": [],
                "next_test": "Photograph the full sheet if total time is needed.",
                "causality_note": "The photo records the result but does not explain why it occurred.",
            }

        with mock.patch.object(gateway, "VISION_ENABLED", True), mock.patch(
            "gateway.openclaw_completion", side_effect=fake_completion
        ):
            response = gateway.call_openclaw(request)

        self.assertEqual(1, len(calls))
        self.assertEqual(
            intelligence_router.SINGLE_AGENT_TEST_SPEC["route"], calls[0][0]
        )
        self.assertIsInstance(calls[0][1][-1]["content"], list)
        payload = json.loads(calls[0][1][-1]["content"][0]["text"])
        self.assertEqual(
            "answer_from_official_results_and_session_review", payload["task"]
        )
        self.assertEqual(
            "racebox-session-review-v4",
            payload["whole_run_deterministic_review"]["contract"],
        )
        self.assertTrue(payload["combine_sources_for_the_question"])
        self.assertIn("clearly visible per-driver lap times", payload["allowed_facts"])
        self.assertEqual(
            1,
            len(
                [
                    item
                    for item in calls[0][1][-1]["content"]
                    if item.get("type") == "image_url"
                ]
            ),
        )
        self.assertEqual(gateway.VISION_PIPELINE, response["pipeline"])
        self.assertTrue(response["routing"]["brain_skipped"])
        self.assertIsNone(response["routing"]["brain"])
        self.assertTrue(response["official_results_photo"]["used"])
        self.assertFalse(response["official_results_photo"]["retained"])

    def test_race_day_v1_remains_backward_compatible_without_memory(self):
        value = gateway.validate_race_day_request(
            {
                "contract": "racebox-race-day-request-v1",
                "question": "What changed?",
                "previous": {"context": {}, "analytics_csv": synthetic_csv()},
                "current": {"context": {}, "analytics_csv": synthetic_csv()},
            }
        )
        self.assertEqual([], value["prior_setup_results"])

    def test_session_chat_reviews_every_lap_without_raw_csv_in_model_prompt(self):
        value = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "Did the car fade as the run went on?",
                "session": {
                    "context": {"run": {"label": "A1"}},
                    "analytics_csv": synthetic_csv(),
                },
                "history": [
                    {"role": "user", "content": f"question {index}"}
                    for index in range(20)
                ],
            }
        )
        self.assertEqual("session_chat", value["mode"])
        self.assertEqual(10, len(value["history"]))
        payload, analytics = gateway._prepare_evidence(value)
        self.assertEqual("one_run_all_laps", payload["review"])
        self.assertNotIn("analytics_csv", str(payload))
        self.assertEqual("racebox-session-review-v5", analytics["contract"])
        self.assertEqual(5, analytics["formula_version"])
        self.assertEqual(3, analytics["run_summary"]["complete_laps"])
        self.assertEqual(3, len(analytics["all_laps"]))
        self.assertEqual(
            [1, 2, 3],
            [lap["raw_lap"] for lap in analytics["all_laps"]],
        )
        self.assertFalse(
            intelligence_router.assess_task(value, analytics)["ultra_eligible"]
        )

    def test_racebox_only_corner_line_and_slowing_review(self):
        review = telemetry_analytics.analyze_session(
            synthetic_line_and_slowing_csv()
        )
        self.assertEqual("racebox-session-review-v5", review["contract"])
        self.assertEqual(5, review["formula_version"])
        self.assertFalse(review["quality"]["radio_controls_available"])
        self.assertTrue(review["quality"]["all_lap_corner_analysis_available"])
        self.assertTrue(review["quality"]["driving_line_analysis_available"])
        self.assertTrue(
            review["quality"]["speed_derived_slowing_analysis_available"]
        )

        analysis = review["corner_analysis"]
        self.assertEqual("measured", analysis["status"])
        self.assertEqual(3, analysis["best_raw_lap"])
        self.assertEqual(4, analysis["detected_corner_count"])
        self.assertEqual("measured", analysis["driving_line_quality"]["status"])
        corner = min(
            analysis["corners"],
            key=lambda item: abs(item["apex_progress_percent"] - 37.5),
        )
        self.assertLess(corner["best_minus_typical_corner_time_s"], 0.0)
        self.assertIsNotNone(corner["best_minus_typical_minimum_speed_kmh"])
        self.assertIsNotNone(corner["best_minus_typical_average_corner_speed_kmh"])
        self.assertIsNotNone(corner["best_minus_typical_exit_speed_kmh"])
        self.assertEqual("measured_difference", corner["driving_line"]["status"])
        self.assertIn(
            corner["driving_line"]["best_lap_apex_position_vs_typical"],
            ("inside", "outside"),
        )
        self.assertGreater(
            abs(corner["driving_line"]["best_lap_apex_shift_vs_typical_m"]),
            0.30,
        )
        self.assertEqual(
            "earlier_or_farther_before_apex",
            corner["speed_derived_slowing"]["status"],
        )
        self.assertGreater(
            corner["speed_derived_slowing"][
                "best_minus_typical_onset_distance_m"
            ],
            0.50,
        )
        self.assertGreater(
            corner["speed_derived_slowing"]["best_minus_typical_onset_time_s"],
            0.0,
        )
        self.assertIn(
            "exact control is inferred",
            corner["speed_derived_slowing"]["interpretation"],
        )
        self.assertEqual("inferred", corner["driver_action_inference"]["status"])
        self.assertIn(
            corner["driver_action_inference"]["label"],
            (
                "likely_earlier_lift_or_short_coast",
                "likely_earlier_lift_or_light_brake",
                "likely_earlier_braking_or_abrupt_lift",
            ),
        )
        self.assertGreaterEqual(corner["driver_action_inference"]["confidence"], 68)
        self.assertGreater(len(corner["corner_trace"]["points"]), 10)
        self.assertEqual(
            "measured_difference",
            corner["driving_line"]["full_corner_path"]["status"],
        )
        self.assertNotIn("latitude", json.dumps(review))
        self.assertNotIn("longitude", json.dumps(review))

    def test_first_eight_lap_scope_survives_follow_up_and_excludes_late_fast_lap(self):
        value = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "What did I do differently in turn 2 of the fast lap?",
                "session": {
                    "context": {"run": {"label": "A Main"}},
                    "analytics_csv": synthetic_line_and_slowing_csv(
                        lap_count=10, late_fast_lap=True
                    ),
                },
                "history": [
                    {
                        "role": "user",
                        "content": "For the first 8 laps, what did I do better on my best lap?",
                    },
                    {"role": "assistant", "content": "Earlier answer."},
                ],
            }
        )
        payload, review = gateway._prepare_evidence(value)
        self.assertEqual(list(range(1, 9)), review["question_scope"]["selected_race_laps"])
        self.assertEqual("recent_driver_context", review["question_scope"]["source"])
        self.assertEqual(3, review["corner_analysis"]["best_raw_lap"])
        self.assertNotIn(10, review["corner_analysis"]["comparison_raw_laps"])
        self.assertEqual(
            "race laps 1-8",
            payload["whole_run_deterministic_review"]["question_scope"]["description"],
        )
        self.assertEqual(
            ["corner-02"],
            payload["whole_run_deterministic_review"]["trace_focus"][
                "detailed_corner_ids"
            ],
        )
        unfocused = [
            corner
            for corner in payload["whole_run_deterministic_review"]["corner_analysis"]["corners"]
            if corner["corner_id"] != "corner-02"
        ]
        self.assertTrue(
            all(
                corner["corner_trace"]["status"] == "summarized_not_focused"
                for corner in unfocused
            )
        )

    def test_stored_companion_review_uses_processed_first_eight_scope(self):
        stored = telemetry_analytics.analyze_session(
            synthetic_line_and_slowing_csv(lap_count=10, late_fast_lap=True),
            include_scope_library=True,
        )
        self.assertIn("8", stored["stored_prefix_lap_scopes"])
        focused = telemetry_analytics.apply_question_scope_to_stored_review(
            stored,
            "For the first 8 laps, what did I do differently?",
        )
        self.assertNotIn("stored_prefix_lap_scopes", focused)
        self.assertEqual("matched_processed_prefix_scope", focused["stored_scope_resolution"])
        self.assertEqual(list(range(1, 9)), focused["question_scope"]["selected_race_laps"])
        self.assertEqual(3, focused["corner_analysis"]["best_raw_lap"])
        self.assertNotIn("analytics_csv", json.dumps(focused))

    def test_natural_line_question_routes_to_correlation_with_v3_evidence(self):
        value = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "What did I do better in that round, including my driving line, and did I start slowing sooner?",
                "session": {
                    "context": {"run": {"label": "A1"}},
                    "analytics_csv": synthetic_line_and_slowing_csv(),
                },
            }
        )
        payload, review = gateway._prepare_evidence(value)
        self.assertNotIn("analytics_csv", str(payload))
        assessment = intelligence_router.assess_task(value, review)
        self.assertTrue(assessment["asks_correlation"])
        self.assertEqual("correlation", assessment["minimum_lane"])
        self.assertEqual(4, assessment["detected_corner_count"])
        self.assertTrue(assessment["driving_line_analysis_available"])

    def test_implausible_local_line_shift_is_withheld(self):
        review = telemetry_analytics.analyze_session(
            synthetic_line_and_slowing_csv(line_change_m=5.0)
        )
        corner = min(
            review["corner_analysis"]["corners"],
            key=lambda item: abs(item["apex_progress_percent"] - 37.5),
        )
        self.assertEqual("alignment_outlier", corner["driving_line"]["status"])
        self.assertIsNone(
            corner["driving_line"]["best_lap_apex_shift_vs_typical_m"]
        )
        self.assertIn("three-metre", corner["driving_line"]["untrusted_reason"])

    def test_racebox_only_whole_run_finds_one_off_stop_and_counts_every_later_lap(self):
        review = telemetry_analytics.analyze_session(
            synthetic_whole_run_incident_csv()
        )
        self.assertEqual("racebox-session-review-v5", review["contract"])
        self.assertEqual(17, review["run_summary"]["complete_laps"])
        self.assertEqual(17, len(review["all_laps"]))
        self.assertFalse(review["quality"]["radio_controls_available"])
        self.assertTrue(review["quality"]["racebox_only_analysis_available"])
        self.assertIn("lap timing", review["quality"]["available_without_sanwa"])

        self.assertEqual(1, len(review["incident_candidates"]))
        candidate = review["incident_candidates"][0]
        self.assertEqual(8, candidate["raw_lap"])
        self.assertEqual(7, candidate["race_lap"])
        self.assertAlmostEqual(94.28, candidate["lap_progress_percent"], delta=0.30)
        self.assertAlmostEqual(3.77, candidate["minimum_speed_kmh"], places=2)
        self.assertGreaterEqual(candidate["confidence"], 80)
        self.assertIn("does not prove a crash", candidate["interpretation"])

        consequence = review["incident_review"]
        self.assertEqual("measured", consequence["status"])
        self.assertEqual([2, 3, 4, 5, 6, 7], consequence["baseline_raw_laps"])
        self.assertEqual(list(range(9, 19)), consequence["post_event_raw_laps"])
        self.assertEqual(10, consequence["post_event_complete_laps"])
        self.assertAlmostEqual(
            0.76, consequence["impact_lap_delta_vs_pre_event_pace_s"], places=2
        )
        self.assertAlmostEqual(
            6.76,
            consequence["post_event_cumulative_delta_vs_pre_event_pace_s"],
            places=2,
        )
        self.assertAlmostEqual(
            7.52, consequence["total_impact_through_finish_delta_s"], places=2
        )
        self.assertEqual(
            "impact lap plus every later complete lap through the end of the run",
            consequence["scope"],
        )

    def test_repeated_tight_corner_stop_is_not_called_a_one_off_incident(self):
        review = telemetry_analytics.analyze_session(
            synthetic_whole_run_incident_csv(repeated_stop=True)
        )
        self.assertEqual([], review["incident_candidates"])
        self.assertEqual("no_candidate", review["incident_review"]["status"])

    def test_unexplained_incident_asks_driver_but_reported_wall_contact_does_not(self):
        unexplained = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "Review the whole run.",
                "session": {
                    "context": {"run": {"label": "A-main"}},
                    "analytics_csv": synthetic_whole_run_incident_csv(),
                },
            }
        )
        payload, review = gateway._prepare_evidence(unexplained)
        self.assertEqual(
            "needs_driver_context", review["incident_context"]["status"]
        )
        self.assertTrue(review["incident_context"]["ask_driver"])
        self.assertIn(
            "Did the car hit something",
            review["incident_context"]["clarification_question"],
        )
        self.assertNotIn("analytics_csv", str(payload))
        assessment = intelligence_router.assess_task(unexplained, review)
        self.assertEqual("correlation", assessment["minimum_lane"])
        self.assertEqual(1, assessment["incident_candidate_count"])

        reported = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "How much time did I lose?",
                "session": {
                    "context": {
                        "run": {"label": "A-main"},
                        "post_run_driver_feel": (
                            "I hit a wall and damaged the right-front body."
                        ),
                    },
                    "analytics_csv": synthetic_whole_run_incident_csv(),
                },
            }
        )
        _, reported_review = gateway._prepare_evidence(reported)
        self.assertEqual(
            "reported_by_driver", reported_review["incident_context"]["status"]
        )
        self.assertFalse(reported_review["incident_context"]["ask_driver"])

    def test_compressed_older_driver_context_remains_driver_confirmed(self):
        review = telemetry_analytics.analyze_session(
            synthetic_whole_run_incident_csv()
        )
        full_history = [
            {
                "role": "user",
                "content": "Stopping to have the marshal fix the body damage was an option.",
            },
            {"role": "assistant", "content": "Understood."},
        ]
        for index in range(10):
            full_history.append(
                {
                    "role": "user" if index % 2 == 0 else "assistant",
                    "content": f"later turn {index}",
                }
            )
        request = {
            "mode": "session_chat",
            "contract": "racebox-session-chat-request-v1",
            "question": "Did you review the data?",
            "session": {"context": {}, "review": review},
            "prior_setup_results": [],
            "history": gateway.bounded_companion_history(full_history),
        }
        _, prepared = gateway._prepare_evidence(request)
        self.assertEqual(
            "reported_by_driver", prepared["incident_context"]["status"]
        )
        self.assertFalse(prepared["incident_context"]["ask_driver"])

    def test_incident_answer_uses_correlation_lane_and_exposes_whole_run_math(self):
        request = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "I hit the wall. How much time did I lose through the finish?",
                "session": {
                    "context": {"run": {"label": "A-main"}},
                    "analytics_csv": synthetic_whole_run_incident_csv(),
                },
            }
        )
        calls = []

        def fake_completion(route, messages, timeout):
            calls.append((route, messages))
            if route == intelligence_router.ROUTER_SPEC["route"]:
                return {
                    "task_type": "incident consequence",
                    "lane": "correlation",
                    "confidence": 95,
                    "reasons": ["The whole remaining run must be interpreted."],
                }
            worker_payload = json.loads(messages[-1]["content"])
            review = worker_payload["whole_run_deterministic_review"]
            self.assertEqual(17, len(review["all_laps"]))
            self.assertEqual(
                "reported_by_driver", review["incident_context"]["status"]
            )
            self.assertAlmostEqual(
                7.52,
                review["incident_review"]["total_impact_through_finish_delta_s"],
                places=2,
            )
            return {
                "verdict": "supported",
                "confidence": 75,
                "summary": "The incident and later pace added 7.52 seconds through the finish.",
                "observations": [],
                "confounds": ["The later loss is associated with the impact, not causal proof."],
                "next_test": "Inspect the body and compare the next undamaged run.",
                "causality_note": "The timing change is measured; damage causation comes from the driver's report.",
            }

        with mock.patch("gateway.openclaw_completion", side_effect=fake_completion):
            response = gateway.call_openclaw(request)
        self.assertEqual("single_agent", response["routing"]["selected_lane"])
        self.assertEqual("openai/gpt-5.6-sol", response["model"])
        self.assertEqual("xhigh", response["thinking"])
        self.assertTrue(response["routing"]["brain_skipped"])
        self.assertEqual(1, response["evidence_summary"]["possible_incident_candidates"])
        self.assertFalse(response["evidence_summary"]["radio_controls_available"])
        self.assertTrue(
            response["evidence_summary"]["racebox_only_analysis_available"]
        )
        self.assertAlmostEqual(
            7.52,
            response["evidence_summary"]["impact_through_finish_delta_s"],
            places=2,
        )
        self.assertEqual(1, len(calls))

    def test_session_chat_requires_the_whole_run_csv(self):
        with self.assertRaisesRegex(ValueError, "session analytics_csv is required"):
            gateway.validate_session_chat_request(
                {
                    "contract": "racebox-session-chat-request-v1",
                    "question": "Which lap was best?",
                    "session": {"context": {}},
                }
            )

    def test_session_chat_rejects_wrong_version_and_oversized_csv(self):
        with self.assertRaisesRegex(ValueError, "unsupported session-chat"):
            gateway.validate_session_chat_request(
                {
                    "contract": "racebox-session-chat-request-v0",
                    "question": "Which lap was best?",
                    "session": {"context": {}, "analytics_csv": synthetic_csv()},
                }
            )
        with self.assertRaisesRegex(ValueError, "too large"):
            gateway.validate_session_chat_request(
                {
                    "contract": "racebox-session-chat-request-v1",
                    "question": "Which lap was best?",
                    "session": {
                        "context": {},
                        "analytics_csv": "x" * 6_000_001,
                    },
                }
            )

    def test_vehicle_dynamics_analytics_match_inputs_and_bound_claims(self):
        result = telemetry_analytics.analyze_pair(
            synthetic_csv(),
            synthetic_csv(
                lateral_extra=0.05,
                speed_step=0.30,
                braking_indicator=True,
            ),
        )
        self.assertEqual("racebox-setup-analytics-v3", result["contract"])
        self.assertEqual(3, result["formula_version"])
        self.assertGreater(
            result["lateral_response"]["matched_lateral_response_delta_g"], 0.03
        )
        self.assertGreater(
            result["forward_bite"]["matched_speed_derived_acceleration_delta_g"],
            0.0,
        )
        self.assertGreaterEqual(
            result["braking"]["current"]["possible_lockup_or_low_grip_indicators"],
            1,
        )
        self.assertIn("straight_speed", result)
        self.assertEqual("measured", result["straight_speed"]["status"])
        self.assertIn(
            result["straight_speed"]["attribution"],
            {
                "corner_exit_or_line",
                "acceleration_or_power_delivery",
                "mixed_or_driving_line",
                "no_measured_change",
            },
        )
        self.assertIn("corner_balance", result)
        self.assertEqual("measured", result["corner_balance"]["status"])
        self.assertIn("overdriving", result)
        self.assertIn(
            "median_brake_response_delay_s",
            result["braking"]["current"],
        )
        self.assertIn(
            "wheel-speed",
            result["braking"]["current"]["interpretation"],
        )
        self.assertGreaterEqual(result["quality"]["confidence"], 70)

        overdrive_result = telemetry_analytics.analyze_pair(
            synthetic_csv(corner_profile="clean"),
            synthetic_csv(corner_profile="overdrive"),
        )
        self.assertEqual(
            "more_overdriving_no_lap_gain",
            overdrive_result["overdriving"]["status"],
        )
        self.assertGreater(
            overdrive_result["overdriving"]["overdriving_index_delta"], 8.0
        )

        roll_result = telemetry_analytics.analyze_pair(
            synthetic_csv(corner_profile="roll_flat"),
            synthetic_csv(corner_profile="roll_more"),
        )
        self.assertEqual(
            "more_chassis_roll_signature",
            roll_result["chassis_roll"]["status"],
        )
        self.assertGreater(roll_result["chassis_roll"]["chassis_roll_delta_deg"], 4.0)

        roll_rate_result = telemetry_analytics.analyze_pair(
            synthetic_csv(corner_profile="roll_rate_slow"),
            synthetic_csv(corner_profile="roll_rate_fast"),
        )
        self.assertEqual("faster_roll_build", roll_rate_result["roll_rate"]["status"])
        self.assertGreater(roll_rate_result["roll_rate"]["roll_rate_delta_dps"], 35.0)
        self.assertGreaterEqual(
            roll_rate_result["roll_rate"]["current_roll_rate_samples"], 25
        )

    def test_track_shape_allows_translation_but_rejects_layout_mismatch(self):
        translated = telemetry_analytics.analyze_pair(
            synthetic_csv(),
            synthetic_csv(latitude_offset=0.000015),
        )
        track = translated["quality"]["track_compatibility"]
        self.assertEqual("compatible", track["status"])
        self.assertGreater(track["translation_m"], 1.0)
        self.assertLess(track["residual_rms_m"], 0.1)

        with self.assertRaisesRegex(ValueError, "same track layout"):
            telemetry_analytics.analyze_pair(
                synthetic_csv(),
                synthetic_csv(track_warp=0.00010),
            )

    def test_language_question_can_use_low_lane(self):
        assessment = intelligence_router.assess_task(
            {
                "mode": "lap",
                "question": "What does matched lateral response mean in plain language?",
                "evidence": {"confidence": 85},
            }
        )
        self.assertEqual("language", assessment["minimum_lane"])
        decision = intelligence_router.normalize_brain_decision(
            {
                "task_type": "definition",
                "lane": "language",
                "confidence": 96,
                "reasons": ["No correlation is requested."],
            },
            assessment,
        )
        self.assertEqual("language", decision["selected_lane"])
        self.assertFalse(decision["ultra_used"])

    def test_ultra_is_denied_when_evidence_is_not_repeatable(self):
        assessment = intelligence_router.assess_task(
            {
                "mode": "lap",
                "question": "Did this setup change cause a reliable gain?",
                "evidence": {"confidence": 99},
            }
        )
        decision = intelligence_router.normalize_brain_decision(
            {"lane": "ultra", "confidence": 99, "reasons": []},
            assessment,
        )
        self.assertEqual("correlation", decision["selected_lane"])
        self.assertFalse(decision["ultra_used"])
        self.assertTrue(
            any("denied" in reason.casefold() for reason in decision["reasons"])
        )

    def test_ultra_is_eligible_only_for_high_trust_complex_correlation(self):
        analytics = telemetry_analytics.analyze_pair(
            synthetic_csv(),
            synthetic_csv(
                lateral_extra=0.05,
                speed_step=0.30,
                braking_indicator=True,
            ),
        )
        request = {
            "mode": "race_day",
            "question": (
                "Is this a reliable setup correlation or only a consequential gain?"
            ),
            "previous": {
                "context": {
                    "conditions": {
                        "track_temperature_c": 24,
                        "tire_runs_before": 1,
                        "battery_pack": "A",
                    }
                }
            },
            "current": {
                "context": {
                    "conditions": {
                        "track_temperature_c": 32,
                        "tire_runs_before": 5,
                        "battery_pack": "B",
                    }
                }
            },
        }
        assessment = intelligence_router.assess_task(request, analytics)
        self.assertGreaterEqual(assessment["quality_confidence"], 85)
        self.assertGreaterEqual(assessment["complexity_score"], 6)
        self.assertTrue(assessment["ultra_eligible"])
        decision = intelligence_router.normalize_brain_decision(
            {"lane": "ultra", "confidence": 88, "reasons": ["Complex conflict."]},
            assessment,
        )
        self.assertEqual("ultra", decision["selected_lane"])
        self.assertTrue(decision["ultra_used"])

    def test_ultra_also_requires_measured_benchmark_gain_over_xhigh(self):
        assessment = {
            "maximum_lane": "ultra",
            "ultra_eligible": True,
            "ultra_denial_reasons": [],
        }
        denied = gateway.apply_runtime_ultra_policy(
            assessment,
            {
                "available": True,
                "fresh": True,
                "recommended_lane": "correlation",
                "score": 100,
                "ultra_approved": False,
            },
        )
        self.assertFalse(denied["ultra_eligible"])
        self.assertEqual("correlation", denied["maximum_lane"])
        self.assertTrue(
            any("quality gain" in reason for reason in denied["ultra_denial_reasons"])
        )
        approved = gateway.apply_runtime_ultra_policy(
            assessment,
            {
                "available": True,
                "fresh": True,
                "recommended_lane": "ultra",
                "score": 100,
                "ultra_approved": True,
            },
        )
        self.assertTrue(approved["ultra_eligible"])
        self.assertEqual("ultra", approved["maximum_lane"])

    def test_low_quality_caps_the_maximum_lane_and_fallback_never_uses_ultra(self):
        analytics = telemetry_analytics.analyze_pair(
            synthetic_csv(),
            synthetic_csv(lateral_extra=0.05, speed_step=0.30),
        )
        analytics["quality"]["confidence"] = 45
        request = {
            "mode": "race_day",
            "question": "Did the setup cause a reliable change?",
            "previous": {"context": {}},
            "current": {"context": {}},
        }
        assessment = intelligence_router.assess_task(request, analytics)
        self.assertFalse(assessment["ultra_eligible"])
        self.assertEqual("correlation", assessment["maximum_lane"])
        self.assertNotEqual(
            "ultra", intelligence_router.deterministic_fallback_lane(assessment)
        )

    def test_legacy_adaptive_pipeline_can_be_used_for_rollback_comparison(self):
        request = gateway.validate_request(
            {
                "setup_change": "none",
                "question": "What does matched lateral response mean in plain language?",
                "evidence": {
                    "contract": "racebox-crew-chief-evidence-v1",
                    "confidence": 85,
                },
            }
        )
        calls = []

        def fake_completion(route, messages, timeout):
            calls.append(route)
            if route == intelligence_router.ROUTER_SPEC["route"]:
                return {
                    "task_type": "definition",
                    "lane": "language",
                    "confidence": 95,
                    "reasons": ["Definition only."],
                }
            return {
                "verdict": "inconclusive",
                "confidence": 80,
                "summary": "This is a definition, not a setup verdict.",
                "observations": [],
                "confounds": [],
                "next_test": "Collect a controlled comparison.",
                "causality_note": "This explanation does not prove causation.",
            }

        with mock.patch("gateway.openclaw_completion", side_effect=fake_completion):
            response = gateway.call_openclaw(request, pipeline_mode="adaptive")
        self.assertEqual(
            [
                intelligence_router.ROUTER_SPEC["route"],
                intelligence_router.LANE_SPECS["language"]["route"],
            ],
            calls,
        )
        self.assertEqual("language", response["routing"]["selected_lane"])
        self.assertEqual("openai/gpt-5.6-luna", response["model"])
        self.assertEqual("low", response["thinking"])

    def test_default_pipeline_skips_brain_and_keeps_coaching_prompt(self):
        request = gateway.validate_session_chat_request(
            {
                "contract": "racebox-session-chat-request-v1",
                "question": "What did I do best and how can I go faster?",
                "session": {"context": {}, "analytics_csv": synthetic_csv()},
            }
        )
        calls = []

        def fake_completion(route, messages, timeout):
            calls.append((route, messages))
            self.assertEqual(gateway.SYSTEM_PROMPT, messages[0]["content"])
            return {
                "verdict": "mixed",
                "confidence": 75,
                "summary": "Turn 2 retained the best corner speed.",
                "observations": [
                    {
                        "area": "Turn 2",
                        "change": "The quicker line was associated with a gain.",
                        "meaning": (
                            "Suggestion to test: use one smooth steering arc, lift "
                            "briefly, then feed throttle as steering unwinds; watch "
                            "minimum speed and corner time."
                        ),
                        "evidence_ids": ["corner:2"],
                    }
                ],
                "confounds": ["Transmitter inputs were not recorded."],
                "next_test": "Repeat the same line for three clean laps.",
                "causality_note": "This is an association, not proof of cause.",
            }

        with mock.patch("gateway.openclaw_completion", side_effect=fake_completion):
            response = gateway.call_openclaw(request)
        self.assertEqual(
            [intelligence_router.SINGLE_AGENT_TEST_SPEC["route"]],
            [route for route, _ in calls],
        )
        self.assertEqual(
            gateway.PIPELINE_NAME, response["pipeline"]
        )
        self.assertEqual("single_agent", response["routing"]["selected_lane"])
        self.assertEqual("single_agent_direct", response["routing"]["source"])
        self.assertTrue(response["routing"]["brain_skipped"])
        self.assertEqual(0.0, response["routing"]["brain_latency_ms"])
        self.assertEqual("openai/gpt-5.6-sol", response["model"])
        self.assertEqual("xhigh", response["thinking"])

    def test_model_usage_keeps_only_numeric_token_diagnostics(self):
        usage = gateway.sanitize_model_usage(
            {
                "prompt_tokens": 100,
                "completion_tokens": "25",
                "total_tokens": 125,
                "completion_tokens_details": {
                    "reasoning_tokens": 20,
                    "unsafe_text": "do not retain",
                },
                "request_id": "not-token-usage",
            }
        )
        self.assertEqual(100, usage["prompt_tokens"])
        self.assertEqual(25, usage["completion_tokens"])
        self.assertEqual(125, usage["total_tokens"])
        self.assertEqual(
            20, usage["completion_tokens_details"]["reasoning_tokens"]
        )
        self.assertNotIn("request_id", usage)
        self.assertNotIn(
            "unsafe_text", usage["completion_tokens_details"]
        )

    def test_race_day_response_returns_storable_evidence_and_memory_ids(self):
        request = gateway.validate_race_day_request(
            {
                "contract": "racebox-race-day-request-v3",
                "question": "Did the setup change make a reliable improvement?",
                "previous": {"context": {}, "analytics_csv": synthetic_csv()},
                "current": {
                    "context": {},
                    "analytics_csv": synthetic_csv(lateral_extra=0.03),
                },
                "prior_setup_results": [
                    {
                        "record_id": "setup-result-spring",
                        "track_name": "RC Raceway",
                        "setup_change": "rear spring changed",
                        "summary": "Earlier result was mixed.",
                        "confidence": 75,
                    }
                ],
            }
        )

        def fake_completion(route, messages, timeout):
            if route == intelligence_router.ROUTER_SPEC["route"]:
                return {
                    "task_type": "setup correlation",
                    "lane": "correlation",
                    "confidence": 90,
                    "reasons": ["Historical evidence must be reconciled."],
                }
            return {
                "verdict": "mixed",
                "confidence": 80,
                "summary": "The response improved locally, but a reliable lap gain was not retained.",
                "observations": [],
                "confounds": ["One previous/current pair is not causal proof."],
                "next_test": "Repeat A/B/A.",
                "causality_note": "The comparison shows association, not causation.",
            }

        with mock.patch("gateway.openclaw_completion", side_effect=fake_completion):
            response = gateway.call_openclaw(request, pipeline_mode="adaptive")
        self.assertEqual(
            ["setup-result-spring"], response["prior_setup_result_ids"]
        )
        self.assertEqual(
            "racebox-setup-analytics-v3",
            response["evidence_summary"]["analytics_contract"],
        )
        self.assertEqual(
            0.03,
            response["evidence_summary"]["lateral_response_delta_g"],
        )
        self.assertIn("straight_speed_attribution", response["evidence_summary"])
        self.assertIn("brake_response_status", response["evidence_summary"])
        self.assertIn("corner_balance_status", response["evidence_summary"])
        self.assertIn("overdriving_status", response["evidence_summary"])
        self.assertIn("chassis_roll_status", response["evidence_summary"])
        self.assertIn("roll_rate_status", response["evidence_summary"])

    def test_brain_failure_uses_deterministic_non_ultra_fallback(self):
        request = gateway.validate_request(
            {
                "setup_change": "rear spring change",
                "question": "Review the measured result.",
                "evidence": {
                    "contract": "racebox-crew-chief-evidence-v1",
                    "confidence": 80,
                },
            }
        )
        calls = []

        def fake_completion(route, messages, timeout):
            calls.append(route)
            if len(calls) == 1:
                raise RuntimeError("brain unavailable")
            return {
                "verdict": "mixed",
                "confidence": 70,
                "summary": "The evidence is mixed.",
                "observations": [],
                "confounds": [],
                "next_test": "Repeat A/B/A.",
                "causality_note": "One comparison is association, not causation.",
            }

        with mock.patch("gateway.openclaw_completion", side_effect=fake_completion):
            response = gateway.call_openclaw(request, pipeline_mode="adaptive")
        self.assertEqual("deterministic_fallback", response["routing"]["source"])
        self.assertNotEqual("ultra", response["routing"]["selected_lane"])
        self.assertFalse(response["routing"]["ultra_used"])

    def test_benchmark_prefers_lowest_passing_lane(self):
        task = benchmark_lanes.TASKS[0]
        results = [
            {
                "task": task["id"],
                "lane": "language",
                "model": "openai/gpt-5.6-luna",
                "thinking": "low",
                "latency_ms": 1000,
                "grade": {"score": 100, "passed": True},
            },
            {
                "task": task["id"],
                "lane": "explanation",
                "model": "openai/gpt-5.6-terra",
                "thinking": "medium",
                "latency_ms": 500,
                "grade": {"score": 100, "passed": True},
            },
        ]
        recommendation = benchmark_lanes.recommendation_for(task, results)
        self.assertEqual("language", recommendation["recommended_lane"])

    def test_setup_memory_benchmark_uses_sol_xhigh_as_baseline(self):
        task = next(
            item
            for item in benchmark_lanes.TASKS
            if item["id"] == "setup_memory_recommendation"
        )
        self.assertEqual("correlation", task["baseline_lane"])
        baseline = intelligence_router.LANE_SPECS[task["baseline_lane"]]
        self.assertEqual("openai/gpt-5.6-sol", baseline["model"])
        self.assertEqual("xhigh", baseline["thinking"])
        report = {
            "verdict": "mixed",
            "confidence": 82,
            "observations": [
                {"evidence_ids": list(task["required_ids"])}
            ],
            "causality_note": "This is an association and does not prove cause.",
        }
        agreement = benchmark_lanes.baseline_agreement(task, report, report)
        self.assertTrue(agreement["passed"])


if __name__ == "__main__":
    unittest.main()
