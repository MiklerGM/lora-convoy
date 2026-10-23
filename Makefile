# Everything that can be checked without a phone or a radio. CI runs the parts as separate jobs
# (.github/workflows/check.yml); `make check` runs them all.
check: check-android check-firmware check-nrf check-tools
	@echo "check: app tests, lint and build; firmware tests, simulator and both images; airtime and route cross-checks"

check-android:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug

# The host tests, the simulator's checks and the Heltec image.
check-firmware:
	@$(MAKE) -s -C firmware test esp

# The Wio Tracker L1 Pro image: needs the Zephyr workspace (firmware/zephyr/setup.sh).
check-nrf:
	@$(MAKE) -s -C firmware nrf

check-tools:
	@python3 -I tools/cross_check.py >/dev/null

.PHONY: check check-android check-firmware check-nrf check-tools
