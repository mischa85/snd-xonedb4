//
//  ContentView.swift
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 01/06/24.
//  Copyright © 2024 Hackerman. All rights reserved.
//

import SwiftUI
import SystemExtensions

struct ContentView : View {
    var body: some View {
        VStack {
            Text("XoneDB4App")
            HStack {
                Button(action: ExtensionManager.shared.activate) {
                    Text("Activate")
                }
                Button(action: ExtensionManager.shared.deactivate) {
                    Text("Deactivate")
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}


#if DEBUG
struct ContentView_Previews : PreviewProvider {
    static var previews: some View {
        ContentView()
    }
}
#endif
