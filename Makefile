# Everything that can be checked without a phone or a radio.
check:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
	@$(MAKE) -s -C firmware test esp nrf
	@python3 -I tools/cross_check.py >/dev/null
	@echo "check: app tests, lint and build; firmware tests, simulator and both images; airtime and route cross-checks"

.PHONY: check
