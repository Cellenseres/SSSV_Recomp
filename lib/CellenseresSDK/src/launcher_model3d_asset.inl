bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        return false;
    }

    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good();
}

const cgltf_accessor* find_attr(const cgltf_primitive& pr, cgltf_attribute_type ty, int idx = 0) {
    for (cgltf_size i = 0; i < pr.attributes_count; i++) {
        const auto& a = pr.attributes[i];
        if (a.type == ty && a.index == idx) {
            return a.data;
        }
    }

    return nullptr;
}

bool image_bytes(const cgltf_image* img, const std::filesystem::path& dir, std::vector<uint8_t>& out) {
    if (!img) {
        return false;
    }

    if (img->buffer_view) {
        const auto* view = img->buffer_view;
        const uint8_t* p = view->data
            ? reinterpret_cast<const uint8_t*>(view->data)
            : ((view->buffer && view->buffer->data) ? (reinterpret_cast<const uint8_t*>(view->buffer->data) + view->offset) : nullptr);
        if (!p || view->size == 0) {
            return false;
        }

        out.assign(p, p + view->size);
        return true;
    }

    if (!img->uri) {
        return false;
    }

    std::string uri = img->uri;
    if (uri.rfind("data:", 0) == 0) {
        const size_t pos = uri.find("base64,");
        if (pos == std::string::npos) {
            return false;
        }

        const char* b64 = uri.c_str() + pos + 7;
        const size_t n = std::strlen(b64);
        const size_t pad = (n && b64[n - 1] == '=') + (n > 1 && b64[n - 2] == '=');
        const cgltf_size out_n = (n / 4) * 3 - pad;

        void* decoded = nullptr;
        cgltf_options options{};
        if (cgltf_load_buffer_base64(&options, out_n, b64, &decoded) != cgltf_result_success || decoded == nullptr) {
            return false;
        }

        out.assign(reinterpret_cast<uint8_t*>(decoded), reinterpret_cast<uint8_t*>(decoded) + out_n);
        std::free(decoded);
        return true;
    }

    std::vector<char> mutable_uri(uri.begin(), uri.end());
    mutable_uri.push_back('\0');
    cgltf_decode_uri(mutable_uri.data());
    return read_file(dir / mutable_uri.data(), out);
}

bool tex_bytes(const cgltf_texture_view& tv, const std::filesystem::path& dir, std::vector<uint8_t>& out) {
    return tv.texture && tv.texture->image && image_bytes(tv.texture->image, dir, out);
}

bool load_glb(const std::filesystem::path& p, CpuModel& out, std::string& err) {
    cgltf_options o{};
    cgltf_data* d = nullptr;
    if (cgltf_parse_file(&o, p.string().c_str(), &d) != cgltf_result_success || !d) {
        err = "parse failed";
        return false;
    }

    auto fin = [&]() {
        if (d) {
            cgltf_free(d);
        }
        d = nullptr;
    };

    if (cgltf_load_buffers(&o, d, p.string().c_str()) != cgltf_result_success) {
        err = "load buffers failed";
        fin();
        return false;
    }

    const cgltf_primitive* pr = nullptr;
    const cgltf_node* owner_node = nullptr;
    for (cgltf_size n = 0; n < d->nodes_count && !pr; n++) {
        const cgltf_node& node = d->nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        for (cgltf_size i = 0; i < node.mesh->primitives_count; i++) {
            const cgltf_primitive& c = node.mesh->primitives[i];
            if (c.type == cgltf_primitive_type_triangles && find_attr(c, cgltf_attribute_type_position)) {
                pr = &c;
                owner_node = &node;
                break;
            }
        }
    }

    if (!pr) {
        for (cgltf_size m = 0; m < d->meshes_count && !pr; m++) {
            for (cgltf_size i = 0; i < d->meshes[m].primitives_count; i++) {
                const cgltf_primitive& c = d->meshes[m].primitives[i];
                if (c.type == cgltf_primitive_type_triangles && find_attr(c, cgltf_attribute_type_position)) {
                    pr = &c;
                    break;
                }
            }
        }
    }

    if (!pr) {
        err = "no triangle primitive";
        fin();
        return false;
    }

    auto* pa = find_attr(*pr, cgltf_attribute_type_position);
    auto* na = find_attr(*pr, cgltf_attribute_type_normal);
    auto* ta = find_attr(*pr, cgltf_attribute_type_tangent);
    auto* ua = find_attr(*pr, cgltf_attribute_type_texcoord, 0);
    if (!pa || pa->count == 0) {
        err = "no positions";
        fin();
        return false;
    }

    M4 node_transform = I();
    if (owner_node != nullptr) {
        cgltf_float world_col_major[16]{};
        cgltf_node_transform_world(owner_node, world_col_major);
        for (int i = 0; i < 16; i++) {
            node_transform.m[i] = static_cast<float>(world_col_major[i]);
        }
    }

    auto transform_pos = [&](const V3& p3) -> V3 {
        V4 v = mul_row_vec({ p3.x, p3.y, p3.z, 1.0f }, node_transform);
        return { v.x, v.y, v.z };
    };
    auto transform_dir = [&](const V3& d3) -> V3 {
        V4 v = mul_row_vec({ d3.x, d3.y, d3.z, 0.0f }, node_transform);
        return norm({ v.x, v.y, v.z });
    };

    const float m00 = node_transform.m[0];
    const float m01 = node_transform.m[1];
    const float m02 = node_transform.m[2];
    const float m10 = node_transform.m[4];
    const float m11 = node_transform.m[5];
    const float m12 = node_transform.m[6];
    const float m20 = node_transform.m[8];
    const float m21 = node_transform.m[9];
    const float m22 = node_transform.m[10];
    const float det3 =
        m00 * (m11 * m22 - m12 * m21) -
        m01 * (m10 * m22 - m12 * m20) +
        m02 * (m10 * m21 - m11 * m20);
    const bool flip_winding = det3 < 0.0f;
    out.had_owner_node = (owner_node != nullptr);
    out.owner_node_det3 = det3;
    out.flipped_winding = flip_winding;
    out.source_had_normals = (na != nullptr);

    const size_t vc = static_cast<size_t>(pa->count);
    out.vertices.resize(vc);
    std::array<float, 4> v{};
    for (size_t i = 0; i < vc; i++) {
        cgltf_accessor_read_float(pa, i, v.data(), 3);
        out.vertices[i].p = transform_pos({ v[0], v[1], v[2] });
        if (na) {
            cgltf_accessor_read_float(na, i, v.data(), 3);
            out.vertices[i].n = transform_dir({ v[0], v[1], v[2] });
        }
        if (ta) {
            cgltf_accessor_read_float(ta, i, v.data(), 4);
            V3 t_dir = transform_dir({ v[0], v[1], v[2] });
            out.vertices[i].t = { t_dir.x, t_dir.y, t_dir.z, v[3] };
        }
        if (ua) {
            cgltf_accessor_read_float(ua, i, v.data(), 2);
            out.vertices[i].uv = { v[0], v[1] };
        }
    }

    if (pr->indices && pr->indices->count) {
        out.indices.resize(static_cast<size_t>(pr->indices->count));
        for (size_t i = 0; i < out.indices.size(); i++) {
            out.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(pr->indices, i));
        }
    } else {
        out.indices.resize(vc);
        for (size_t i = 0; i < vc; i++) {
            out.indices[i] = static_cast<uint32_t>(i);
        }
    }

    if (flip_winding) {
        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            std::swap(out.indices[i + 1], out.indices[i + 2]);
        }
    }

    if (!na) {
        std::vector<V3> accum(vc, V3{ 0.0f, 0.0f, 0.0f });
        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            uint32_t i0 = out.indices[i + 0];
            uint32_t i1 = out.indices[i + 1];
            uint32_t i2 = out.indices[i + 2];
            if (i0 >= vc || i1 >= vc || i2 >= vc) {
                continue;
            }
            V3 p0 = out.vertices[i0].p;
            V3 p1 = out.vertices[i1].p;
            V3 p2 = out.vertices[i2].p;
            V3 e1 = sub(p1, p0);
            V3 e2 = sub(p2, p0);
            V3 fn = cross(e1, e2);
            accum[i0] = { accum[i0].x + fn.x, accum[i0].y + fn.y, accum[i0].z + fn.z };
            accum[i1] = { accum[i1].x + fn.x, accum[i1].y + fn.y, accum[i1].z + fn.z };
            accum[i2] = { accum[i2].x + fn.x, accum[i2].y + fn.y, accum[i2].z + fn.z };
        }
        for (size_t i = 0; i < vc; i++) {
            const float len2 = dot(accum[i], accum[i]);
            out.vertices[i].n = (len2 > 1e-12f) ? norm(accum[i]) : V3{ 0.0f, 0.0f, 1.0f };
        }
        out.generated_smooth_normals = true;
    }

    if (pr->material) {
        const auto* m = pr->material;
        if (m->has_pbr_metallic_roughness) {
            out.base_color = {
                m->pbr_metallic_roughness.base_color_factor[0],
                m->pbr_metallic_roughness.base_color_factor[1],
                m->pbr_metallic_roughness.base_color_factor[2],
                m->pbr_metallic_roughness.base_color_factor[3]
            };
            tex_bytes(m->pbr_metallic_roughness.base_color_texture, p.parent_path(), out.albedo);
        }
        tex_bytes(m->normal_texture, p.parent_path(), out.normal);
        if (m->has_specular) {
            out.spec_factor = m->specular.specular_factor;
            out.spec_color = {
                m->specular.specular_color_factor[0],
                m->specular.specular_color_factor[1],
                m->specular.specular_color_factor[2],
                1.0f
            };
            if (!tex_bytes(m->specular.specular_color_texture, p.parent_path(), out.spec)) {
                tex_bytes(m->specular.specular_texture, p.parent_path(), out.spec);
            }
        }
    }

    if (!out.vertices.empty()) {
        V3 mn{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        V3 mx{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
        for (const Vertex& vtx : out.vertices) {
            mn.x = std::min(mn.x, vtx.p.x);
            mn.y = std::min(mn.y, vtx.p.y);
            mn.z = std::min(mn.z, vtx.p.z);
            mx.x = std::max(mx.x, vtx.p.x);
            mx.y = std::max(mx.y, vtx.p.y);
            mx.z = std::max(mx.z, vtx.p.z);
        }

        out.bounds_min = mn;
        out.bounds_max = mx;
        out.bounds_center = {
            (mn.x + mx.x) * 0.5f,
            (mn.y + mx.y) * 0.5f,
            (mn.z + mx.z) * 0.5f
        };

        float radius = 0.0f;
        for (const Vertex& vtx : out.vertices) {
            const V3 dpos = sub(vtx.p, out.bounds_center);
            radius = std::max(radius, len(dpos));
        }
        out.bounds_radius = std::max(radius, 0.001f);
    }

    fin();
    return true;
}
