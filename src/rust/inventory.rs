//! Immutable retail resource inventory with checked references to nested PACK members.
use super::archive::{ResourceRef, SourceTree};
use super::{CoreError, CoreResult, models};
use std::collections::{BTreeMap, BTreeSet};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

/// One named PACK member in its original order, including duplicate-name identity.
#[derive(Clone)]
pub struct MemberRef {
    pub alias: String,
    pub name: String,
    pub index: usize,
    pub resource: ResourceRef,
}

/// Top-level disc resources and all nested member references without copied payloads.
pub struct Inventory {
    roots: Vec<ResourceRef>,
    leaves: Vec<ResourceRef>,
    nodes: BTreeMap<String, ResourceRef>,
    members: BTreeMap<String, Vec<MemberRef>>,
}

impl Inventory {
    pub fn build(source: &SourceTree) -> CoreResult<Self> {
        let mut inventory = Self {
            roots: Vec::new(),
            leaves: Vec::new(),
            nodes: BTreeMap::new(),
            members: BTreeMap::new(),
        };
        for name in source.names() {
            let resource = source.resource(name)?;
            inventory.roots.push(resource.clone());
            inventory.walk(resource, 0)?;
        }
        Ok(inventory)
    }

    fn walk(&mut self, resource: ResourceRef, depth: usize) -> CoreResult<()> {
        if depth > 16 {
            return Err(fail("PACK nesting exceeds 16"));
        }
        let alias = resource.alias.clone();
        if self.nodes.insert(alias.clone(), resource.clone()).is_some() {
            return Err(fail(format!("duplicate resource identity: {alias}")));
        }
        let Some(members) = models::pack(&resource.bytes()) else {
            self.leaves.push(resource);
            return Ok(());
        };
        let mut seen = BTreeSet::new();
        let mut children = Vec::with_capacity(members.len());
        for (index, member) in members.into_iter().enumerate() {
            let child_alias = if seen.insert(member.name.clone()) {
                format!("{alias}/{}", member.name)
            } else {
                format!("{alias}/{}#entry:{index}", member.name)
            };
            let end = member
                .start
                .checked_add(member.size)
                .ok_or_else(|| fail("PACK member range overflow"))?;
            let child = resource.subrange(child_alias.clone(), member.start..end)?;
            children.push(MemberRef {
                alias: child_alias,
                name: member.name,
                index,
                resource: child.clone(),
            });
            self.walk(child, depth + 1)?;
        }
        self.members.insert(alias, children);
        Ok(())
    }

    pub fn roots(&self) -> &[ResourceRef] {
        &self.roots
    }

    pub fn leaves(&self) -> &[ResourceRef] {
        &self.leaves
    }

    pub fn get(&self, alias: &str) -> Option<&ResourceRef> {
        self.nodes.get(alias)
    }

    pub fn members(&self, alias: &str) -> Option<&[MemberRef]> {
        self.members.get(alias).map(Vec::as_slice)
    }

    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.nodes.keys().map(String::as_str)
    }
}
