import AppKit
import SceneKit
import SwiftUI

/// A lightweight SceneKit host for the coloured CAD model bundled with the
/// flasher. Scrolling never changes the framing; dragging inspects the board,
/// and an idle showroom turn resumes after release.
struct BoardModelView: NSViewRepresentable {
    var reduceMotion: Bool

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> DragOnlySceneView {
        let view = DragOnlySceneView(frame: .zero)
        view.backgroundColor = .clear
        view.antialiasingMode = .multisampling4X
        view.preferredFramesPerSecond = 60
        view.rendersContinuously = !reduceMotion
        view.allowsCameraControl = false
        view.autoenablesDefaultLighting = false
        view.scene = context.coordinator.makeScene(reduceMotion: reduceMotion)
        view.pointOfView = context.coordinator.cameraNode
        view.onDragBegan = { [weak coordinator = context.coordinator] in
            coordinator?.beginInteraction()
        }
        view.onDrag = { [weak coordinator = context.coordinator] horizontal, vertical in
            coordinator?.rotateFromDrag(horizontal: horizontal, vertical: vertical)
        }
        view.onDragEnded = { [weak coordinator = context.coordinator] in
            coordinator?.endInteraction()
        }
        return view
    }

    func updateNSView(_ view: DragOnlySceneView, context: Context) {
        view.rendersContinuously = !reduceMotion
        context.coordinator.setRotationEnabled(!reduceMotion)
    }

    final class Coordinator {
        private let spinKey = "board-spin"
        private let rockKey = "board-rock"
        private weak var spinNode: SCNNode?
        private weak var rockNode: SCNNode?
        private weak var inspectionNode: SCNNode?
        private var motionAllowed = true
        private var resumeWorkItem: DispatchWorkItem?
        fileprivate private(set) var cameraNode: SCNNode?

        func makeScene(reduceMotion: Bool) -> SCNScene {
            let scene = SCNScene()

            let board = SCNNode()
            board.name = "ESP32 board"
            if let url = Bundle.module.url(forResource: "ESP32Board", withExtension: "usdz"),
               let model = try? SCNScene(url: url, options: [.checkConsistency: true]) {
                for child in model.rootNode.childNodes {
                    board.addChildNode(child.clone())
                }
                centreAndScale(board)
            } else {
                // This is intentionally quiet: a missing optional preview must
                // never prevent the firmware tool itself from opening.
                board.addChildNode(fallbackBoard())
            }
            let spinNode = SCNNode()
            spinNode.eulerAngles.z = -0.32
            spinNode.addChildNode(board)

            let rockNode = SCNNode()
            rockNode.eulerAngles.x = 0.07
            rockNode.addChildNode(spinNode)

            let inspectionNode = SCNNode()
            inspectionNode.addChildNode(rockNode)
            scene.rootNode.addChildNode(inspectionNode)
            self.spinNode = spinNode
            self.rockNode = rockNode
            self.inspectionNode = inspectionNode

            let camera = SCNCamera()
            camera.usesOrthographicProjection = true
            camera.orthographicScale = 1.35
            camera.zNear = 0.01
            camera.zFar = 20
            let cameraNode = SCNNode()
            cameraNode.camera = camera
            cameraNode.position = SCNVector3(0, -1.55, 5.2)
            cameraNode.look(at: SCNVector3(0, 0, 0))
            scene.rootNode.addChildNode(cameraNode)
            self.cameraNode = cameraNode

            let ambient = SCNLight()
            ambient.type = .ambient
            ambient.color = NSColor(white: 0.72, alpha: 1)
            ambient.intensity = 520
            let ambientNode = SCNNode()
            ambientNode.light = ambient
            scene.rootNode.addChildNode(ambientNode)

            let key = SCNLight()
            key.type = .directional
            key.color = NSColor(white: 1, alpha: 1)
            key.intensity = 1_150
            let keyNode = SCNNode()
            keyNode.light = key
            keyNode.eulerAngles = SCNVector3(-0.75, 0.45, -0.35)
            scene.rootNode.addChildNode(keyNode)

            let rim = SCNLight()
            rim.type = .directional
            rim.color = NSColor(red: 0.48, green: 0.83, blue: 0.69, alpha: 1)
            rim.intensity = 420
            let rimNode = SCNNode()
            rimNode.light = rim
            rimNode.eulerAngles = SCNVector3(0.45, -2.2, 0)
            scene.rootNode.addChildNode(rimNode)

            setRotationEnabled(!reduceMotion)
            return scene
        }

        func setRotationEnabled(_ enabled: Bool) {
            motionAllowed = enabled
            guard let spinNode, let rockNode else { return }
            if enabled {
                guard spinNode.action(forKey: spinKey) == nil else { return }
                let turn = SCNAction.rotateBy(x: 0, y: 0, z: .pi * 2, duration: 24)
                turn.timingMode = .linear
                spinNode.runAction(.repeatForever(turn), forKey: spinKey)

                let leanForward = SCNAction.rotateBy(x: 0.055, y: 0, z: 0, duration: 3.8)
                let leanBack = SCNAction.rotateBy(x: -0.11, y: 0, z: 0, duration: 7.6)
                let settle = SCNAction.rotateBy(x: 0.055, y: 0, z: 0, duration: 3.8)
                for action in [leanForward, leanBack, settle] {
                    action.timingMode = .easeInEaseOut
                }
                rockNode.runAction(.repeatForever(.sequence([leanForward, leanBack, settle])), forKey: rockKey)
            } else {
                resumeWorkItem?.cancel()
                spinNode.removeAction(forKey: spinKey)
                rockNode.removeAction(forKey: rockKey)
            }
        }

        func beginInteraction() {
            resumeWorkItem?.cancel()
            spinNode?.isPaused = true
            rockNode?.isPaused = true
        }

        func rotateFromDrag(horizontal: CGFloat, vertical: CGFloat) {
            guard let inspectionNode else { return }
            inspectionNode.eulerAngles.z -= horizontal * 0.009
            let nextTilt = inspectionNode.eulerAngles.x + vertical * 0.006
            inspectionNode.eulerAngles.x = min(0.48, max(-0.48, nextTilt))
        }

        func endInteraction() {
            guard motionAllowed else { return }
            let work = DispatchWorkItem { [weak self] in
                self?.spinNode?.isPaused = false
                self?.rockNode?.isPaused = false
            }
            resumeWorkItem = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.9, execute: work)
        }

        private func centreAndScale(_ node: SCNNode) {
            let (minimum, maximum) = node.boundingBox
            let centre = SCNVector3(
                (minimum.x + maximum.x) / 2,
                (minimum.y + maximum.y) / 2,
                (minimum.z + maximum.z) / 2
            )
            let span = max(
                maximum.x - minimum.x,
                maximum.y - minimum.y,
                maximum.z - minimum.z
            )
            guard span > 0 else { return }
            node.pivot = SCNMatrix4MakeTranslation(centre.x, centre.y, centre.z)
            let scale = 2.25 / span
            node.scale = SCNVector3(scale, scale, scale)
        }

        private func fallbackBoard() -> SCNNode {
            let geometry = SCNBox(width: 1.1, height: 0.06, length: 2.2, chamferRadius: 0.035)
            let material = SCNMaterial()
            material.diffuse.contents = NSColor(red: 0.03, green: 0.05, blue: 0.04, alpha: 1)
            material.roughness.contents = 0.36
            geometry.materials = [material]
            return SCNNode(geometry: geometry)
        }
    }
}

/// SceneKit's stock camera controller also treats the scroll wheel as zoom.
/// This view deliberately owns only click-drag rotation so the board never
/// shifts when the user scrolls the surrounding flasher.
final class DragOnlySceneView: SCNView {
    var onDragBegan: (() -> Void)?
    var onDrag: ((CGFloat, CGFloat) -> Void)?
    var onDragEnded: (() -> Void)?
    private var previousPoint: NSPoint?

    override var acceptsFirstResponder: Bool { true }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        previousPoint = convert(event.locationInWindow, from: nil)
        NSCursor.closedHand.set()
        onDragBegan?()
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if let previousPoint {
            onDrag?(point.x - previousPoint.x, point.y - previousPoint.y)
        }
        previousPoint = point
    }

    override func mouseUp(with event: NSEvent) {
        previousPoint = nil
        NSCursor.openHand.set()
        onDragEnded?()
    }

    override func scrollWheel(with event: NSEvent) {
        // Keep the preview's authored framing stable.
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .openHand)
    }
}
