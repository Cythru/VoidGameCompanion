use std::collections::HashMap;

// ── C++ FFI for world state ─────────────────────────────────────────────────

/// Block data returned from the C++ world state manager.
/// The C++ side manages chunk storage, light levels, and biome data.
#[repr(C)]
pub struct CBlockInfo {
    pub block_id:   u32,
    pub block_meta: u8,
    pub light:      u8,
    pub sky_light:  u8,
    pub biome_id:   u32,
}

#[repr(C)]
#[derive(Clone)]
pub struct CEntityInfo {
    pub entity_id:   u32,
    pub entity_type: u32,
    pub x: f64,
    pub y: f64,
    pub z: f64,
    pub yaw: f32,
    pub pitch: f32,
    pub health: f32,
}

extern "C" {
    /// Get block info at world coordinates. Returns false if chunk not loaded.
    fn vf_world_get_block(x: i32, y: i32, z: i32, out: *mut CBlockInfo) -> bool;

    /// Get entities within radius of a position.
    /// Returns count written to `out`. Caller must provide buffer.
    fn vf_world_get_nearby_entities(
        x: f64, y: f64, z: f64,
        radius: f64,
        out: *mut CEntityInfo,
        max_count: usize,
    ) -> usize;

    /// Check if a block position is solid (has collision).
    fn vf_world_is_solid(x: i32, y: i32, z: i32) -> bool;

    /// Load a chunk column into the world state. Called when the server sends chunk data.
    fn vf_world_load_chunk(chunk_x: i32, chunk_z: i32, data: *const u8, data_len: usize) -> bool;

    /// Unload a chunk column.
    fn vf_world_unload_chunk(chunk_x: i32, chunk_z: i32);
}

// ── Safe Rust wrapper ───────────────────────────────────────────────────────

/// Block information at a world position.
#[derive(Debug, Clone, Copy)]
pub struct BlockInfo {
    pub block_id:   u32,
    pub block_meta: u8,
    pub light:      u8,
    pub sky_light:  u8,
    pub biome_id:   u32,
}

/// Entity information.
#[derive(Debug, Clone)]
pub struct EntityInfo {
    pub entity_id:   u32,
    pub entity_type: u32,
    pub x: f64,
    pub y: f64,
    pub z: f64,
    pub yaw: f32,
    pub pitch: f32,
    pub health: f32,
    pub name: Option<String>,
}

/// Safe wrapper around C++ world state via FFI.
pub struct WorldView {
    /// Entity ID -> name mapping (populated from server packets).
    entity_names: HashMap<u32, String>,
}

impl WorldView {
    pub fn new() -> Self {
        Self {
            entity_names: HashMap::new(),
        }
    }

    /// Get block info at world coordinates.
    pub fn get_block(&self, x: i32, y: i32, z: i32) -> Option<BlockInfo> {
        let mut info = CBlockInfo {
            block_id: 0,
            block_meta: 0,
            light: 0,
            sky_light: 0,
            biome_id: 0,
        };

        let loaded = unsafe { vf_world_get_block(x, y, z, &mut info) };
        if loaded {
            Some(BlockInfo {
                block_id:   info.block_id,
                block_meta: info.block_meta,
                light:      info.light,
                sky_light:  info.sky_light,
                biome_id:   info.biome_id,
            })
        } else {
            None
        }
    }

    /// Check if a position is air (block_id == 0 or chunk not loaded).
    pub fn is_air(&self, x: i32, y: i32, z: i32) -> bool {
        self.get_block(x, y, z)
            .map(|b| b.block_id == 0)
            .unwrap_or(true)
    }

    /// Check if a block position is solid (has collision box).
    pub fn is_solid(&self, x: i32, y: i32, z: i32) -> bool {
        unsafe { vf_world_is_solid(x, y, z) }
    }

    /// Get all entities within radius of a position.
    pub fn get_nearby_entities(&self, x: f64, y: f64, z: f64, radius: f64) -> Vec<EntityInfo> {
        const MAX_ENTITIES: usize = 256;
        let mut buffer = vec![CEntityInfo {
            entity_id: 0, entity_type: 0,
            x: 0.0, y: 0.0, z: 0.0,
            yaw: 0.0, pitch: 0.0, health: 0.0,
        }; MAX_ENTITIES];

        let count = unsafe {
            vf_world_get_nearby_entities(
                x, y, z, radius,
                buffer.as_mut_ptr(),
                MAX_ENTITIES,
            )
        };

        buffer.truncate(count);
        buffer.iter().map(|e| EntityInfo {
            entity_id:   e.entity_id,
            entity_type: e.entity_type,
            x: e.x,
            y: e.y,
            z: e.z,
            yaw: e.yaw,
            pitch: e.pitch,
            health: e.health,
            name: self.entity_names.get(&e.entity_id).cloned(),
        }).collect()
    }

    /// Look up a named entity's position.
    pub fn get_entity_position_by_name(&self, name: &str) -> Option<(f64, f64, f64)> {
        let entity_id = self.entity_names.iter()
            .find(|(_, n)| n.as_str() == name)
            .map(|(&id, _)| id)?;

        // Search nearby with a large radius
        let entities = self.get_nearby_entities(0.0, 64.0, 0.0, 10000.0);
        entities.iter()
            .find(|e| e.entity_id == entity_id)
            .map(|e| (e.x, e.y, e.z))
    }

    /// Register an entity name (called when server sends player info).
    pub fn set_entity_name(&mut self, entity_id: u32, name: String) {
        self.entity_names.insert(entity_id, name);
    }

    /// Remove an entity name mapping.
    pub fn remove_entity(&mut self, entity_id: u32) {
        self.entity_names.remove(&entity_id);
    }

    /// Load a chunk from raw server data.
    pub fn load_chunk(&self, chunk_x: i32, chunk_z: i32, data: &[u8]) -> bool {
        unsafe { vf_world_load_chunk(chunk_x, chunk_z, data.as_ptr(), data.len()) }
    }

    /// Unload a chunk.
    pub fn unload_chunk(&self, chunk_x: i32, chunk_z: i32) {
        unsafe { vf_world_unload_chunk(chunk_x, chunk_z) }
    }
}
