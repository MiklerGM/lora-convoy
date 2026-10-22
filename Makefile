# Everything that can be checked without a phone or a radio.
check:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
	@python3 -I tools/cross_check.py >/dev/null
	@echo "check: app tests, lint and build; airtime and route cross-checks"

.PHONY: check
