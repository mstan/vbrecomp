// Source-build checks cannot identify installed package bytes or option values.
// Hash a bounded, path-independent active plan once, when it is committed.
// Unselected packages/resources have no effect on compatibility identity.
static void identity_text(std::vector<uint8_t>& out,const std::string& text) {
    const uint64_t n=text.size();for(int i=0;i<8;++i)out.push_back(uint8_t(n>>(8*i)));
    out.insert(out.end(),text.begin(),text.end());
}
static bool identity_content(const fs::path& root,std::vector<uint8_t>& out,std::string* error) {
    const auto reject=[&](){set_error(error,"Active mod contents are linked, unsupported, unreadable or exceed package limits");return false;};
    std::error_code ec;
    if(root.empty() || is_link(root))return reject();
    if(fs::is_regular_file(root,ec)) {
        std::string hash;if(!sha256_file(root,hash,error))return false;
        identity_text(out,"file");identity_text(out,hash);return true;
    }
    if(ec || !fs::is_directory(root,ec) || ec || fs::absolute(root).lexically_normal()==fs::absolute(root).root_path())return reject();
    std::map<std::string,fs::path> files;unsigned entries=0;uint64_t total=0;
    for(fs::recursive_directory_iterator it(root,ec),end;it!=end && !ec;it.increment(ec)) {
        if(++entries>kMaxArchiveFiles || it.depth()>16 || is_link(it->path()) || !inside(root,it->path()))return reject();
        if(it->is_directory(ec))continue;
        if(ec || !it->is_regular_file(ec) || ec)return reject();
        const auto size=it->file_size(ec);if(ec || size>kMaxArchiveBytes-total)return reject();total+=size;
        files.emplace(it->path().lexically_relative(root).generic_u8string(),it->path());
    }
    if(ec)return reject();
    identity_text(out,"directory");identity_text(out,std::to_string(files.size()));
    for(const auto& [relative,path]:files) {
        std::string hash;if(!sha256_file(path,hash,error))return false;
        identity_text(out,relative);identity_text(out,hash);
    }
    return true;
}
static bool committed_mod_identity(Runtime& runtime,std::array<uint8_t,32>& out,std::string* error) {
    try {
        std::vector<uint8_t> manifest;
        identity_text(manifest,"vb-active-mods-v1");identity_text(manifest,runtime.game_id);
        identity_text(manifest,runtime.rom_sha256);
        for(const auto& entry:runtime.packages) {
            const Package* package=selected_package(runtime,entry.first);if(!package)continue;
            bool active=false;
            for(const auto& feature:package->features)active|=feature_enabled(runtime,*package,feature);
            if(!active)continue;
            identity_text(manifest,"package");identity_text(manifest,package->id);identity_text(manifest,package->version);
            if(!identity_content(package->root,manifest,error))return false;
            for(const auto& feature:package->features) {
                if(!feature_enabled(runtime,*package,feature))continue;
                identity_text(manifest,"feature");identity_text(manifest,feature.id);
                for(const auto& option:package->options)if(option.feature_id==feature.id) {
                    identity_text(manifest,"option");identity_text(manifest,option.id);
                    identity_text(manifest,option_value(runtime,*package,feature,option));
                }
                for(const auto& resource:package->resources)if(resource.feature_id==feature.id) {
                    identity_text(manifest,"resource");identity_text(manifest,resource.id);
                    const auto path=resource_path(runtime,*package,feature,resource);
                    if(path.empty())identity_text(manifest,"unselected");
                    else if(!identity_content(fs::u8path(path),manifest,error))return false;
                }
            }
        }
        vb_sha256_compute(manifest.data(),manifest.size(),out.data());return true;
    } catch(const std::exception&) {
        set_error(error,"Cannot allocate or inspect active mod identity");return false;
    }
}
