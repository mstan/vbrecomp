#if defined(RECOMP_LAUNCHER)
static int choose_launcher(const fs::path& base,bool resume) {
        RecompLauncherCSettings settings = {};
        settings.window_scale = vb_host_config.scale;
        settings.fullscreen = vb_host_config.fullscreen;
        settings.linear_filter = vb_host_config.filter;
        settings.enable_audio = vb_host_config.audio;
        settings.volume = vb_host_config.volume;
        settings.player_src[0] = vb_host_config.player_source;
        settings.deadzone[0] = vb_host_config.deadzone;
        settings.skip_launcher = vb_host_config.skip_launcher;
        std::strncpy(settings.player_gamepad_guid[0], vb_host_config.gamepad_guid.c_str(), 39);
        std::copy_n(vb_host_config.keys, 14, settings.player_key_bind[0]);
        std::copy_n(vb_host_config.pads, 14, settings.player_pad_bind[0]);
        for(unsigned i=1;i<vb_host_player_count();++i) {
            const auto& p=vb_host_player(i);settings.player_src[i]=p.player_source;settings.deadzone[i]=p.deadzone;
            std::strncpy(settings.player_gamepad_guid[i],p.gamepad_guid.c_str(),39);
            std::copy_n(p.keys,14,settings.player_key_bind[i]);std::copy_n(p.pads,14,settings.player_pad_bind[i]);
        }
        RecompLauncherCGameInfo game = {};
        launcher_profile_apply("vb", &game);
        game.num_players=int(vb_host_player_count());
        game.name = VB_GAME_TITLE;
        game.region = VB_GAME_REGION;
        game.expected_crc = VB_GAME_CRC32;
        game.has_expected_crc = VB_GAME_CRC32 != 0;
        game.mods = vb_mod_runtime_launcher_provider_c();
        const std::string config_text = config_path.u8string();
        game.config_path = config_text.c_str();
        vb_netplay_configure_launcher(&game);
        if(resume)vb_netplay_resume_launcher(&game);
        RecompLauncherCSettings defaults = settings;
        const VbHostConfig default_values;
        defaults.window_scale = 2;
        defaults.fullscreen = defaults.linear_filter = defaults.skip_launcher = 0;
        defaults.enable_audio = 1;
        defaults.volume = 100;
        defaults.player_src[0] = 1;
        defaults.deadzone[0] = 35;
        defaults.player_gamepad_guid[0][0] = 0;
        std::copy_n(default_values.keys, 14, defaults.player_key_bind[0]);
        std::copy_n(default_values.pads, 14, defaults.player_pad_bind[0]);
        for(unsigned i=1;i<vb_host_player_count();++i) {
            const auto& p=default_values.extra_players[i-1];defaults.player_src[i]=p.player_source;
            defaults.deadzone[i]=p.deadzone;defaults.player_gamepad_guid[i][0]=0;
            std::copy_n(p.keys,14,defaults.player_key_bind[i]);std::copy_n(p.pads,14,defaults.player_pad_bind[i]);
        }
        game.default_settings = &defaults;
        char selected[4096] = {};
        const int result = recomp_launcher_run_window(VB_GAME_TITLE, &settings, &game,
            (base / "assets").u8string().c_str(), last_rom.c_str(), selected, sizeof(selected));
        if (result == RECOMP_LAUNCHER_RESULT_QUIT) return 0;
        if (result != RECOMP_LAUNCHER_RESULT_LAUNCH) { error = "The launcher could not start."; return -1; }
        last_rom = selected;
        if(!vb_netplay_apply_launch(&settings.netplay_launch)){error=vb_netplay_error();return -1;}
        vb_host_config.scale = std::clamp(settings.window_scale, 1, 6);
        vb_host_config.fullscreen = std::clamp(settings.fullscreen, 0, 2);
        vb_host_config.filter = settings.linear_filter != 0;
        vb_host_config.audio = settings.enable_audio != 0;
        vb_host_config.volume = std::clamp(settings.volume, 0, 100);
        vb_host_config.player_source = settings.player_src[0];
        vb_host_config.deadzone = std::clamp(settings.deadzone[0], 0, 100);
        vb_host_config.skip_launcher = settings.skip_launcher != 0;
        vb_host_config.gamepad_guid = settings.player_gamepad_guid[0];
        std::copy_n(settings.player_key_bind[0], 14, vb_host_config.keys);
        std::copy_n(settings.player_pad_bind[0], 14, vb_host_config.pads);
        for(unsigned i=1;i<vb_host_player_count();++i) {
            auto& p=vb_host_player(i);p.player_source=std::clamp(settings.player_src[i],0,2);
            p.deadzone=std::clamp(settings.deadzone[i],0,100);p.gamepad_guid=settings.player_gamepad_guid[i];
            std::copy_n(settings.player_key_bind[i],14,p.keys);std::copy_n(settings.player_pad_bind[i],14,p.pads);
        }
    return 1;
}
#endif
