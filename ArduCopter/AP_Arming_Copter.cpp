#include "Copter.h"

#pragma GCC diagnostic push
#if defined(__clang_major__) && __clang_major__ >= 14
#pragma GCC diagnostic ignored "-Wbitwise-instead-of-logical"
#endif

bool AP_Arming_Copter::pre_arm_checks(bool display_failure)
{
    const bool passed = run_pre_arm_checks(display_failure);
    set_pre_arm_check(passed);
    return passed;
}

// perform pre-arm checks
//  return true if the checks pass successfully
bool AP_Arming_Copter::run_pre_arm_checks(bool display_failure)
{
    // exit immediately if already armed
    if (copter.motors->armed()) {
        return true;
    }

    if (!hal.scheduler->is_system_initialized()) {
        check_failed(display_failure, "System not initialised");
        return false;
    }

    // check if motor interlock and either Emergency Stop aux switches are used
    // at the same time.  This cannot be allowed.
    bool passed = true;
    if (rc().find_channel_for_option(RC_Channel::AUX_FUNC::MOTOR_INTERLOCK) &&
        (rc().find_channel_for_option(RC_Channel::AUX_FUNC::MOTOR_ESTOP) || 
        rc().find_channel_for_option(RC_Channel::AUX_FUNC::ARM_EMERGENCY_STOP))){
        check_failed(display_failure, "Interlock/E-Stop Conflict");
        passed = false;
    }

    // check if motor interlock aux switch is in use
    // if it is, switch needs to be in disabled position to arm
    // otherwise exit immediately.
    if (copter.ap.using_interlock && copter.ap.motor_interlock_switch) {
        check_failed(display_failure, "Motor Interlock Enabled");
        passed = false;
    }

    if (!disarm_switch_checks(display_failure)) {
        passed = false;
    }

    // always check motors
    char failure_msg[100] {};
    if (!copter.motors->arming_checks(ARRAY_SIZE(failure_msg), failure_msg)) {
        check_failed(display_failure, "Motors: %s", failure_msg);
        passed = false;
    }

#if FRAME_CONFIG == HELI_FRAME && MODE_AUTOROTATE_ENABLED
    // check on autorotation config
    if (!copter.g2.arot.arming_checks(ARRAY_SIZE(failure_msg), failure_msg)) {
        check_failed(display_failure, "AROT: %s", failure_msg);
        passed = false;
    }
#endif

    // If not passed all checks return false
    if (!passed) {
        return false;
    }

    // if pre arm checks are disabled run only the mandatory checks
    if (checks_to_perform == 0) {
        return mandatory_checks(display_failure);
    }

    // bitwise & ensures all checks are run
    return parameter_checks(display_failure)
        & oa_checks(display_failure)
        & gcs_failsafe_check(display_failure)
        & winch_checks(display_failure)
        & rc_throttle_failsafe_checks(display_failure)
        & alt_checks(display_failure)
#if AP_AIRSPEED_ENABLED
        & AP_Arming::airspeed_checks(display_failure)
#endif
        & AP_Arming::pre_arm_checks(display_failure);
}

bool AP_Arming_Copter::rc_throttle_failsafe_checks(bool display_failure) const
{
    if (!check_enabled(Check::RC)) {
        // this check has been disabled
        return true;
    }

    // throttle failsafe.  In this case the parameter also gates the
    // no-pulses RC failure case - the radio-in value can be zero due
    // to not having received any RC pulses at all.  Note that if we
    // have ever seen RC and then we *lose* RC then these checks are
    // likely to pass if the user is relying on no-pulses to detect RC
    // failure.  However, arming is precluded in that case by being in
    // RC failsafe.
    if (copter.g.failsafe_throttle == FS_THR_DISABLED) {
        return true;
    }

#if FRAME_CONFIG == HELI_FRAME
    const char *rc_item = "Collective";
#else
    const char *rc_item = "Throttle";
#endif

    if (!rc().has_had_rc_receiver() && !rc().has_had_rc_override()) {
        check_failed(Check::RC, display_failure, "RC not found");
        return false;
    }

    // check throttle is not too low - must be above failsafe throttle
    if (copter.channel_throttle->get_radio_in() < copter.g.failsafe_throttle_value) {
        check_failed(Check::RC, display_failure, "%s below failsafe", rc_item);
        return false;
    }

    return true;
}

bool AP_Arming_Copter::barometer_checks(bool display_failure)
{
    if (!AP_Arming::barometer_checks(display_failure)) {
        return false;
    }

    bool ret = true;
    // check Baro
    if (check_enabled(Check::BARO)) {
        // Check baro & inav alt are within 1m if EKF is operating in an absolute position mode.
        // Do not check if intending to operate in a ground relative height mode as EKF will output a ground relative height
        // that may differ from the baro height due to baro drift.
        const auto &ahrs = AP::ahrs();
        const bool using_baro_ref = !ahrs.has_status(AP_AHRS::Status::PRED_HORIZ_POS_REL) && ahrs.has_status(AP_AHRS::Status::PRED_HORIZ_POS_ABS);
        float pos_d_m = 0;
        UNUSED_RESULT(AP::ahrs().get_relative_position_D_origin_float(pos_d_m));
        if (using_baro_ref) {
            if (fabsf(-pos_d_m * 100.0 - copter.baro_alt) > PREARM_MAX_ALT_DISPARITY_CM) {
                check_failed(Check::BARO, display_failure, "Altitude disparity");
                ret = false;
            }
        }
    }
    return ret;
}

bool AP_Arming_Copter::ins_checks(bool display_failure)
{
    bool ret = AP_Arming::ins_checks(display_failure);

    if (check_enabled(Check::INS)) {

        // get ekf attitude (if bad, it's usually the gyro biases)
        if (!pre_arm_ekf_attitude_check()) {
            check_failed(Check::INS, display_failure, "EKF attitude is bad");
            ret = false;
        }
    }

    return ret;
}

bool AP_Arming_Copter::board_voltage_checks(bool display_failure)
{
    if (!AP_Arming::board_voltage_checks(display_failure)) {
        return false;
    }

    // check battery voltage
    if (check_enabled(Check::VOLTAGE)) {
        if (copter.battery.has_failsafed()) {
            check_failed(Check::VOLTAGE, display_failure, "Battery failsafe");
            return false;
        }
    }

    return true;
}

// expected to return true if the terrain database is required to have
// all data loaded
bool AP_Arming_Copter::terrain_database_required() const
{

    if (copter.wp_nav->get_terrain_source() == AC_WPNav::TerrainSource::TERRAIN_FROM_RANGEFINDER) {
        // primary terrain source is from rangefinder, allow arming without terrain database
        return false;
    }

    if (copter.wp_nav->get_terrain_source() == AC_WPNav::TerrainSource::TERRAIN_FROM_TERRAINDATABASE &&
        copter.mode_rtl.get_alt_type() == ModeRTL::RTLAltType::TERRAIN) {
        return true;
    }
    return AP_Arming::terrain_database_required();
}

bool AP_Arming_Copter::parameter_checks(bool display_failure)
{
    // check various parameter values
    if (check_enabled(Check::PARAMETERS)) {

        // failsafe parameter checks
        if (copter.g.failsafe_throttle) {
            // check throttle min is above throttle failsafe trigger and that the trigger is above ppm encoder's loss-of-signal value of 900
            if (copter.channel_throttle->get_radio_min() <= copter.g.failsafe_throttle_value+10 || copter.g.failsafe_throttle_value < 910) {
                check_failed(Check::PARAMETERS, display_failure, "Check FS_THR_VALUE");
                return false;
            }
        }
        if (copter.g.failsafe_gcs == FS_GCS_ENABLED_CONTINUE_MISSION) {
            // FS_GCS_ENABLE == 2 has been removed
            check_failed(Check::PARAMETERS, display_failure, "FS_GCS_ENABLE=2 removed, see FS_OPTIONS");
        }

        // lean angle parameter check
        if (copter.aparm.angle_max < 1000 || copter.aparm.angle_max > 8000) {
            check_failed(Check::PARAMETERS, display_failure, "Check ANGLE_MAX");
            return false;
        }

        // acro balance parameter check
#if MODE_ACRO_ENABLED || MODE_SPORT_ENABLED
        if ((copter.g.acro_balance_roll > copter.attitude_control->get_angle_roll_p().kP()) || (copter.g.acro_balance_pitch > copter.attitude_control->get_angle_pitch_p().kP())) {
            check_failed(Check::PARAMETERS, display_failure, "Check ACRO_BAL_ROLL/PITCH");
            return false;
        }
#endif

        // pilot-speed-up parameter check
        if (copter.g.pilot_speed_up <= 0) {
            check_failed(Check::PARAMETERS, display_failure, "Check PILOT_SPEED_UP");
            return false;
        }

        #if FRAME_CONFIG == HELI_FRAME
        char fail_msg[100]{};
        // check input manager parameters
        if (!copter.input_manager.parameter_check(fail_msg, ARRAY_SIZE(fail_msg))) {
            check_failed(Check::PARAMETERS, display_failure, "%s", fail_msg);
            return false;
        }

        // Ensure an Aux Channel is configured for motor interlock
        if (rc().find_channel_for_option(RC_Channel::AUX_FUNC::MOTOR_INTERLOCK) == nullptr) {
            check_failed(Check::PARAMETERS, display_failure, "Motor Interlock not configured");
            return false;
        }

        #else
        switch (copter.g2.frame_class.get()) {
        case AP_Motors::MOTOR_FRAME_HELI_QUAD:
        case AP_Motors::MOTOR_FRAME_HELI_DUAL:
        case AP_Motors::MOTOR_FRAME_HELI:
            check_failed(Check::PARAMETERS, display_failure, "Invalid MultiCopter FRAME_CLASS");
            return false;

        default:
            break;
        }
        #endif // HELI_FRAME

        // checks when using range finder for RTL
#if MODE_RTL_ENABLED
        if (copter.mode_rtl.get_alt_type() == ModeRTL::RTLAltType::TERRAIN) {
            // get terrain source from wpnav
            const char *failure_template = "RTL_ALT_TYPE is above-terrain but %s";
            switch (copter.wp_nav->get_terrain_source()) {
            case AC_WPNav::TerrainSource::TERRAIN_UNAVAILABLE:
                check_failed(Check::PARAMETERS, display_failure, failure_template, "no terrain data");
                return false;
                break;
            case AC_WPNav::TerrainSource::TERRAIN_FROM_RANGEFINDER:
#if AP_RANGEFINDER_ENABLED
                if (!copter.rangefinder_state.enabled || !copter.rangefinder.has_orientation(ROTATION_PITCH_270)) {
                    check_failed(Check::PARAMETERS, display_failure, failure_template, "no rangefinder");
                    return false;
                }
                // check if RTL_ALT is higher than rangefinder's max range
                if (copter.g.rtl_altitude > copter.rangefinder.max_distance_orient(ROTATION_PITCH_270) * 100) {
                    check_failed(Check::PARAMETERS, display_failure, failure_template, "RTL_ALT (in cm) above RNGFND_MAX (in metres)");
                    return false;
                }
#else
                check_failed(Check::PARAMETERS, display_failure, failure_template, "rangefinder not in firmware");
#endif
                break;
            case AC_WPNav::TerrainSource::TERRAIN_FROM_TERRAINDATABASE:
                // these checks are done in AP_Arming
                break;
            }
        }
#endif

        // check adsb avoidance failsafe
#if HAL_ADSB_ENABLED
        if (copter.failsafe.adsb) {
            check_failed(Check::PARAMETERS, display_failure, "ADSB threat detected");
            return false;
        }
#endif

        // ensure controllers are OK with us arming:
        char failure_msg[100] = {};
        if (!copter.pos_control->pre_arm_checks("PSC", failure_msg, ARRAY_SIZE(failure_msg))) {
            check_failed(Check::PARAMETERS, display_failure, "Bad parameter: %s", failure_msg);
            return false;
        }
        if (!copter.attitude_control->pre_arm_checks("ATC", failure_msg, ARRAY_SIZE(failure_msg))) {
            check_failed(Check::PARAMETERS, display_failure, "Bad parameter: %s", failure_msg);
            return false;
        }
    }

    return true;
}

bool AP_Arming_Copter::oa_checks(bool display_failure)
{
#if AP_OAPATHPLANNER_ENABLED
    char failure_msg[100] = {};
    if (copter.g2.oa.pre_arm_check(failure_msg, ARRAY_SIZE(failure_msg))) {
        return true;
    }
    // display failure
    if (strlen(failure_msg) == 0) {
        check_failed(display_failure, "%s", "Check Object Avoidance");
    } else {
        check_failed(display_failure, "%s", failure_msg);
    }
    return false;
#else
    return true;
#endif
}

bool AP_Arming_Copter::rc_calibration_checks(bool display_failure)
{
    const RC_Channel *channels[] = {
        copter.channel_roll,
        copter.channel_pitch,
        copter.channel_throttle,
        copter.channel_yaw
    };

    // bitwise & ensures all checks are run
    copter.ap.pre_arm_rc_check = rc_checks_copter_sub(display_failure, channels)
        & AP_Arming::rc_calibration_checks(display_failure);

    return copter.ap.pre_arm_rc_check;
}

// performs pre_arm gps related checks and returns true if passed
bool AP_Arming_Copter::gps_checks(bool display_failure)
{
    // check if fence requires GPS
    bool fence_requires_gps = false;
#if AP_FENCE_ENABLED
    // if circular or polygon fence is enabled we need GPS
    fence_requires_gps = (copter.fence.get_enabled_fences() & (AC_FENCE_TYPE_CIRCLE | AC_FENCE_TYPE_POLYGON)) > 0;
#endif

    // check if flight mode requires GPS
    bool mode_requires_gps = copter.flightmode->requires_GPS() || fence_requires_gps || (copter.simple_mode == Copter::SimpleMode::SUPERSIMPLE);

    // call parent gps checks
    if (mode_requires_gps) {
        if (!AP_Arming::gps_checks(display_failure)) {
            AP_Notify::flags.pre_arm_gps_check = false;
            return false;
        }
    }

    // run mandatory gps checks first
    if (!mandatory_gps_checks(display_failure)) {
        AP_Notify::flags.pre_arm_gps_check = false;
        return false;
    }

    // return true if GPS is not required
    if (!mode_requires_gps) {
        AP_Notify::flags.pre_arm_gps_check = true;
        return true;
    }

    // return true immediately if gps check is disabled
    if (!check_enabled(Check::GPS)) {
        AP_Notify::flags.pre_arm_gps_check = true;
        return true;
    }

    // warn about hdop separately - to prevent user confusion with no gps lock
    if ((copter.gps.num_sensors() > 0) && (copter.gps.get_hdop() > copter.g.gps_hdop_good)) {
        check_failed(Check::GPS, display_failure, "High GPS HDOP");
        AP_Notify::flags.pre_arm_gps_check = false;
        return false;
    }

    // if we got here all must be ok
    AP_Notify::flags.pre_arm_gps_check = true;
    return true;
}

// check ekf attitude is acceptable
bool AP_Arming_Copter::pre_arm_ekf_attitude_check()
{
    return AP::ahrs().has_status(AP_AHRS::Status::ATTITUDE_VALID);
}

#if HAL_PROXIMITY_ENABLED
// check nothing is too close to vehicle
bool AP_Arming_Copter::proximity_checks(bool display_failure) const
{

    if (!AP_Arming::proximity_checks(display_failure)) {
        return false;
    }

    if (!check_enabled(Check::PARAMETERS)) {
        // check is disabled
        return true;
    }

    // get closest object if we might use it for avoidance
#if AP_AVOIDANCE_ENABLED
    float angle_deg, distance;
    if (copter.avoid.proximity_avoidance_enabled() && copter.g2.proximity.get_closest_object(angle_deg, distance)) {
        // display error if something is within 60cm
        const float tolerance = 0.6f;
        if (distance <= tolerance) {
            check_failed(Check::PARAMETERS, display_failure, "Proximity %d deg, %4.2fm (want > %0.1fm)", (int)angle_deg, (double)distance, (double)tolerance);
            return false;
        }
    }
#endif

    return true;
}
#endif  // HAL_PROXIMITY_ENABLED

// performs mandatory gps checks.  returns true if passed
bool AP_Arming_Copter::mandatory_gps_checks(bool display_failure)
{
    // check if flight mode requires GPS
    bool mode_requires_gps = copter.flightmode->requires_GPS();

    // always check if inertial nav has started and is ready
    const auto &ahrs = AP::ahrs();
    char failure_msg[100] = {};
    if (!ahrs.pre_arm_check(mode_requires_gps, failure_msg, sizeof(failure_msg))) {
        check_failed(display_failure, "AHRS: %s", failure_msg);
        return false;
    }

    // check if fence requires GPS
    bool fence_requires_gps = false;
#if AP_FENCE_ENABLED
    // if circular or polygon fence is enabled we need GPS
    fence_requires_gps = (copter.fence.get_enabled_fences() & (AC_FENCE_TYPE_CIRCLE | AC_FENCE_TYPE_POLYGON)) > 0;
#endif

    if (mode_requires_gps || require_location == RequireLocation::YES) {
        if (!copter.position_ok()) {
            // vehicle level position estimate checks
            check_failed(display_failure, "Need Position Estimate");
            return false;
        }
    } else if (fence_requires_gps) {
        if (!copter.position_ok()) {
            // clarify to user why they need GPS in non-GPS flight mode
            check_failed(display_failure, "Fence enabled, need position estimate");
            return false;
        }
    } else {
        // return true if GPS is not required
        return true;
    }

    // check for GPS glitch (as reported by EKF)
    nav_filter_status filt_status;
    if (ahrs.get_filter_status(filt_status)) {
        if (filt_status.flags.gps_glitching) {
            check_failed(display_failure, "GPS glitching");
            return false;
        }
    }

    // check EKF's compass, position, height and velocity variances are below failsafe threshold
    if (copter.g.fs_ekf_thresh > 0.0f) {
        float vel_variance, pos_variance, hgt_variance, tas_variance;
        Vector3f mag_variance;
        ahrs.get_variances(vel_variance, pos_variance, hgt_variance, mag_variance, tas_variance);
        const struct {
            const char *name;
            float value;
        } variances[] {
            { "compass", mag_variance.length() },
            { "position", pos_variance },
            { "velocity", vel_variance },
            { "height", hgt_variance },
        };
        for (auto &variance : variances) {
            if (variance.value < copter.g.fs_ekf_thresh) {
                continue;
            }
            check_failed(display_failure, "EKF %s variance", variance.name);
            return false;
        }
    }

    // if we got here all must be ok
    return true;
}

// Check GCS failsafe
bool AP_Arming_Copter::gcs_failsafe_check(bool display_failure)
{
    if (copter.failsafe.gcs) {
        check_failed(display_failure, "GCS failsafe on");
        return false;
    }
    return true;
}

// check winch
bool AP_Arming_Copter::winch_checks(bool display_failure) const
{
#if AP_WINCH_ENABLED
    // pass if parameter or all arming checks disabled
    if (!check_enabled(Check::PARAMETERS)) {
        return true;
    }

    const AP_Winch *winch = AP::winch();
    if (winch == nullptr) {
        return true;
    }
    char failure_msg[100] = {};
    if (!winch->pre_arm_check(failure_msg, sizeof(failure_msg))) {
        check_failed(display_failure, "%s", failure_msg);
        return false;
    }
#endif
    return true;
}

// performs altitude checks.  returns true if passed
bool AP_Arming_Copter::alt_checks(bool display_failure)
{
    // always EKF altitude estimate
    if (!copter.flightmode->has_manual_throttle() && !copter.ekf_alt_ok()) {
        check_failed(display_failure, "Need Alt Estimate");
        return false;
    }

    return true;
}

// arm_checks - perform final checks before arming
//  always called just before arming.  Return true if ok to arm
//  has side-effect that logging is started
bool AP_Arming_Copter::arm_checks(AP_Arming::Method method)
{
    const auto &ahrs = AP::ahrs();

    // always check if inertial nav has started and is ready
    if (!ahrs.healthy()) {
        check_failed(true, "AHRS not healthy");
        return false;
    }

#ifndef ALLOW_ARM_NO_COMPASS
    // if non-compass is source of heading we can skip compass health check
    if (!ahrs.using_noncompass_for_yaw()) {
        const Compass &_compass = AP::compass();
        // check compass health
        if (!_compass.healthy()) {
            check_failed(true, "Compass not healthy");
            return false;
        }
    }
#endif

    // always check if the current mode allows arming
    if (!copter.flightmode->allows_arming(method)) {
        check_failed(true, "%s mode not armable", copter.flightmode->name());
        return false;
    }

    // succeed if arming checks are disabled
    if (checks_to_perform == 0) {
        return true;
    }

    // check lean angle
    if (check_enabled(Check::INS)) {
        if (degrees(acosf(ahrs.cos_roll()*ahrs.cos_pitch()))*100.0f > copter.aparm.angle_max) {
            check_failed(Check::INS, true, "Leaning");
            return false;
        }
    }

    // check adsb
#if HAL_ADSB_ENABLED
    if (check_enabled(Check::PARAMETERS)) {
        if (copter.failsafe.adsb) {
            check_failed(Check::PARAMETERS, true, "ADSB threat detected");
            return false;
        }
    }
#endif

    // check throttle
    if (check_enabled(Check::RC)) {
#if FRAME_CONFIG == HELI_FRAME
        const char *rc_item = "Collective";
#else
        const char *rc_item = "Throttle";
#endif
        // check throttle is not too high - skips checks if arming from GCS/scripting in Guided,Guided_NoGPS or Auto 
        if (!((AP_Arming::method_is_GCS(method) || method == AP_Arming::Method::SCRIPTING) && copter.flightmode->allows_GCS_or_SCR_arming_with_throttle_high())) {
            // above top of deadband is too always high
            if (copter.get_pilot_desired_climb_rate() > 0.0f) {
                check_failed(Check::RC, true, "%s too high", rc_item);
                return false;
            }
            // in manual modes throttle must be at zero
#if FRAME_CONFIG != HELI_FRAME
            if ((copter.flightmode->has_manual_throttle() || copter.flightmode->mode_number() == Mode::Number::DRIFT) && copter.channel_throttle->get_control_in() > 0) {
                check_failed(Check::RC, true, "%s too high", rc_item);
                return false;
            }
#endif
        }
    }

    // check if safety switch has been pushed
    if (hal.util->safety_switch_state() == AP_HAL::Util::SAFETY_DISARMED) {
        check_failed(true, "Safety Switch");
        return false;
    }

    // superclass method should always be the last thing called; it
    // has side-effects which would need to be cleaned up if one of
    // our arm checks failed
    return AP_Arming::arm_checks(method);
}

// mandatory checks that will be run if ARMING_CHECK is zero or arming forced
bool AP_Arming_Copter::mandatory_checks(bool display_failure)
{
    // call mandatory gps checks and update notify status because regular gps checks will not run
    bool result = mandatory_gps_checks(display_failure);
    AP_Notify::flags.pre_arm_gps_check = result;

    // call mandatory alt check
    if (!alt_checks(display_failure)) {
        result = false;
    }

    return result & AP_Arming::mandatory_checks(display_failure);
}

void AP_Arming_Copter::set_pre_arm_check(bool b)
{
    copter.ap.pre_arm_check = b;
    AP_Notify::flags.pre_arm_check = b;
}

bool AP_Arming_Copter::arm(const AP_Arming::Method method, const bool do_arming_checks)
{
    static bool in_arm_motors = false;

    // exit immediately if already in this function
    if (in_arm_motors) {
        return false;
    }
    in_arm_motors = true;

    // return true if already armed
    if (copter.motors->armed()) {
        in_arm_motors = false;
        return true;
    }

    if (!AP_Arming::arm(method, do_arming_checks)) {
        AP_Notify::events.arming_failed = true;
        in_arm_motors = false;
        return false;
    }

#if HAL_LOGGING_ENABLED
    // let logger know that we're armed (it may open logs e.g.)
    AP::logger().set_vehicle_armed(true);
#endif

    // disable cpu failsafe because initialising everything takes a while
    copter.failsafe_disable();

    // notify that arming will occur (we do this early to give plenty of warning)
    AP_Notify::flags.armed = true;
    // call notify update a few times to ensure the message gets out
    for (uint8_t i=0; i<=10; i++) {
        AP::notify().update();
    }

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    send_arm_disarm_statustext("Arming motors");
#endif

    // Remember Orientation
    // --------------------
    copter.init_simple_bearing();

    auto &ahrs = AP::ahrs();

    copter.initial_armed_bearing_rad = ahrs.get_yaw_rad();

    if (!ahrs.home_is_set()) {
        // Reset EKF altitude if home hasn't been set yet (we use EKF altitude as substitute for alt above home)
        ahrs.resetHeightDatum();
        LOGGER_WRITE_EVENT(LogEvent::EKF_ALT_RESET);

        // we have reset height, so arming height is zero
        copter.arming_altitude_m = 0;
    } else if (!ahrs.home_is_locked()) {
        // Reset home position if it has already been set before (but not locked)
        if (!copter.set_home_to_current_location(false)) {
            // ignore failure
        }

        // remember the height when we armed (ignore failures)
        float pos_d_m = 0;
        UNUSED_RESULT(AP::ahrs().get_relative_position_D_origin_float(pos_d_m));
        copter.arming_altitude_m = -pos_d_m;
    }
    copter.update_super_simple_bearing(false);

    // Reset SmartRTL return location. If activated, SmartRTL will ultimately try to land at this point
#if MODE_SMARTRTL_ENABLED
    copter.g2.smart_rtl.set_home(copter.position_ok());
#endif

    hal.util->set_soft_armed(true);

#if HAL_SPRAYER_ENABLED
    // turn off sprayer's test if on
    copter.sprayer.test_pump(false);
#endif

    // output lowest possible value to motors
    copter.motors->output_min();

    // finally actually arm the motors
    copter.motors->armed(true);

#if HAL_LOGGING_ENABLED
    // log flight mode in case it was changed while vehicle was disarmed
    AP::logger().Write_Mode((uint8_t)copter.flightmode->mode_number(), copter.control_mode_reason);
#endif

    // re-enable failsafe
    copter.failsafe_enable();

    // perf monitor ignores delay due to arming
    AP::scheduler().perf_info.ignore_this_loop();

    // flag exiting this function
    in_arm_motors = false;

    // Log time stamp of arming event
    copter.arm_time_ms = millis();

    // Start the arming delay
    copter.ap.in_arming_delay = true;

    // assumed armed without a arming, switch. Overridden in switches.cpp
    copter.ap.armed_with_airmode_switch = false;

    // return success
    return true;
}

// arming.disarm - disarm motors
bool AP_Arming_Copter::disarm(const AP_Arming::Method method, bool do_disarm_checks)
{
    // return immediately if we are already disarmed
    if (!copter.motors->armed()) {
        return true;
    }

    // do not allow disarm via mavlink if we think we are flying:
    if (do_disarm_checks &&
        AP_Arming::method_is_GCS(method) &&
        !copter.ap.land_complete) {
        return false;
    }

    if (method == AP_Arming::Method::RUDDER) {
        if (!copter.flightmode->has_manual_throttle() && !copter.ap.land_complete) {
            return false;
        }
    }

    if (!AP_Arming::disarm(method, do_disarm_checks)) {
        return false;
    }

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    send_arm_disarm_statustext("Disarming motors");
#endif

    auto &ahrs = AP::ahrs();

    // save compass offsets learned by the EKF if enabled
    Compass &compass = AP::compass();
    if (ahrs.use_compass() && compass.get_learn_type() == Compass::LearnType::COPY_FROM_EKF) {
        for(uint8_t i=0; i<COMPASS_MAX_INSTANCES; i++) {
            Vector3f magOffsets;
            if (ahrs.getMagOffsets(i, magOffsets)) {
                compass.set_and_save_offsets(i, magOffsets);
            }
        }
    }

    // we are not in the air
    copter.set_land_complete(true);
    copter.set_land_complete_maybe(true);

    // send disarm command to motors
    copter.motors->armed(false);

#if MODE_AUTO_ENABLED
    // reset the mission
    copter.mode_auto.mission.reset();
#endif

#if HAL_LOGGING_ENABLED
    AP::logger().set_vehicle_armed(false);
#endif

    hal.util->set_soft_armed(false);

    copter.ap.in_arming_delay = false;

#if AUTOTUNE_ENABLED
    // Possibly save auto tuned parameters
    copter.mode_autotune.autotune.disarmed(copter.flightmode == &copter.mode_autotune);
#endif

    return true;
}

#pragma GCC diagnostic pop
