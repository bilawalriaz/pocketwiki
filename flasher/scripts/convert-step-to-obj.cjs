#!/usr/bin/env node

// Tessellate a coloured STEP assembly with OpenCascade and emit an OBJ/MTL
// pair that Blender can turn into the USDZ shipped by the macOS flasher.
//
// Usage:
//   npm install --prefix /tmp/esp-model-tools occt-import-js@0.0.23
//   NODE_PATH=/tmp/esp-model-tools/node_modules \
//     node scripts/convert-step-to-obj.cjs input.step output.obj

const fs = require("node:fs");
const path = require("node:path");
const occtImport = require("occt-import-js");

const [, , inputPath, outputPath] = process.argv;
if (!inputPath || !outputPath) {
  console.error("usage: convert-step-to-obj.cjs input.step output.obj");
  process.exit(2);
}

const clampByte = (value) => Math.max(0, Math.min(255, Math.round(value * 255)));
const colorKey = (color) => color.map(clampByte).join("_");
const materialName = (color) => `step_${colorKey(color)}`;

async function main() {
  const occt = await occtImport();
  const result = occt.ReadStepFile(fs.readFileSync(inputPath), {
    linearUnit: "millimeter",
    linearDeflectionType: "bounding_box_ratio",
    linearDeflection: 0.001,
    angularDeflection: 0.35,
  });
  if (!result.success) throw new Error("OpenCascade could not read the STEP file");

  const objLines = [`mtllib ${path.basename(outputPath, path.extname(outputPath))}.mtl`];
  const materials = new Map();
  let vertexOffset = 1;
  let triangleCount = 0;

  result.meshes.forEach((mesh, meshIndex) => {
    const positions = mesh.attributes.position.array;
    const normals = mesh.attributes.normal?.array;
    const indices = mesh.index.array;
    const baseColor = mesh.color ?? [0.72, 0.74, 0.72];
    const faceColors = new Array(indices.length / 3).fill(baseColor);

    for (const face of mesh.brep_faces ?? []) {
      if (!face.color) continue;
      for (let triangle = face.first; triangle <= face.last; triangle += 1) {
        faceColors[triangle] = face.color;
      }
    }

    objLines.push(`o mesh_${meshIndex}_${String(mesh.name || "part").replace(/\s+/g, "_")}`);
    for (let i = 0; i < positions.length; i += 3) {
      objLines.push(`v ${positions[i]} ${positions[i + 1]} ${positions[i + 2]}`);
    }
    if (normals) {
      for (let i = 0; i < normals.length; i += 3) {
        objLines.push(`vn ${normals[i]} ${normals[i + 1]} ${normals[i + 2]}`);
      }
    }

    let activeMaterial = "";
    for (let triangle = 0; triangle < indices.length / 3; triangle += 1) {
      const color = faceColors[triangle];
      const nextMaterial = materialName(color);
      if (nextMaterial !== activeMaterial) {
        objLines.push(`usemtl ${nextMaterial}`);
        activeMaterial = nextMaterial;
        materials.set(nextMaterial, color);
      }
      const a = vertexOffset + indices[triangle * 3];
      const b = vertexOffset + indices[triangle * 3 + 1];
      const c = vertexOffset + indices[triangle * 3 + 2];
      objLines.push(normals ? `f ${a}//${a} ${b}//${b} ${c}//${c}` : `f ${a} ${b} ${c}`);
    }

    vertexOffset += positions.length / 3;
    triangleCount += indices.length / 3;
  });

  const mtlLines = [];
  for (const [name, color] of materials) {
    mtlLines.push(
      `newmtl ${name}`,
      `Kd ${color[0]} ${color[1]} ${color[2]}`,
      "Ka 0 0 0",
      "Ks 0.16 0.16 0.16",
      "Ns 80",
      "d 1",
      "illum 2",
      "",
    );
  }

  fs.writeFileSync(outputPath, `${objLines.join("\n")}\n`);
  fs.writeFileSync(outputPath.replace(/\.obj$/i, ".mtl"), `${mtlLines.join("\n")}\n`);
  console.log(JSON.stringify({
    meshes: result.meshes.length,
    triangles: triangleCount,
    materials: materials.size,
  }));
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
