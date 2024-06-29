//
//  ExtensionDelegate.swift
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 01/06/24.
//  Copyright © 2024 Hackerman. All rights reserved.
//

import Foundation
import SystemExtensions
import os.log

class ExtensionManager : NSObject, OSSystemExtensionRequestDelegate {
    
    static let shared = ExtensionManager()
    
    func activate() {
        os_log("sysex activation request for %@", "sc.hackerman.xonedb4driver")
        let activationRequest = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: "sc.hackerman.xonedb4driver", queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }
    
    func deactivate() {
        let activationRequest = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: "sc.hackerman.xonedb4driver", queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }
    
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        os_log("sysex actionForReplacingExtension %@ %@", existing, ext)
        
        return .replace
    }
    
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        os_log("sysex needsUserApproval")
        
    }
    
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        os_log("sysex didFinishWithResult %@", result.rawValue)
        
    }
    
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        os_log("sysex didFailWithError %@", error.localizedDescription)
    }
}
