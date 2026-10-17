"""Local fakes of the e2e harness: an RTMP ingest and a silent TCP sink.

Every child process started here uses CREATE_NO_WINDOW so the harness never flashes a console.
"""
import subprocess

NO_WINDOW = subprocess.CREATE_NO_WINDOW
