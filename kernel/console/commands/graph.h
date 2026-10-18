#ifndef CONSOLE_CMD_GRAPH_H
#define CONSOLE_CMD_GRAPH_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/lib/graphix.h>

int console_cmd_graph(int arg_count, char** args) {
    int8_t vertexBuffer[] = {
        -10, -10, -10,   10, -10, -10,   10,  10, -10,  3, // Front face
         10,  10, -10,  -10,  10, -10,  -10, -10, -10,  3, 
         10, -10, -10,   10, -10,  10,   10,  10,  10,  2, // Right face
         10,  10,  10,   10,  10, -10,   10, -10, -10,  2, 
         10, -10,  10,  -10, -10,  10,  -10,  10,  10,  3, // Back face
        -10,  10,  10,   10,  10,  10,   10, -10,  10,  3, 
        -10, -10,  10,  -10, -10, -10,  -10,  10, -10,  2, // Left face
        -10,  10, -10,  -10,  10,  10,  -10, -10,  10,  2, 
        -10,  10, -10,   10,  10, -10,   10,  10,  10,  2, // Top face
         10,  10,  10,  -10,  10,  10,  -10,  10, -10,  2, 
        -10, -10, -10,  -10, -10,  10,   10, -10,  10,  2, // Bottom face
         10, -10,  10,   10, -10, -10,  -10, -10, -10,  2 , 
    };
    
    gl_init(GL_MODE_GRAPHICS);
    glClear(0);
    
    glBufferData(vertexBuffer, sizeof(vertexBuffer));
    
    // Fire up the timer
    //interrupt_enable();
    
    glUniformRotation(0.0f, 0.0f, 0.0f);
    //glUniformScale(100.0f);
    
    float rotationX=0.0f;
    float rotationY=0.0f;
    float rotationZ=0.0f;
    
    glUniformScale(9.0f);
    
    //uint64_t lastTime = time_ms();
    
    while(1) {
        
        //uint64_t currentTime = time_ms();
        //uint64_t delta = currentTime - lastTime;
        
        //if (delta < 4) 
        //    continue;
        //lastTime = currentTime;
        
        // Update
        rotationX += 1.0f;
        rotationY += 1.4f;
        rotationZ += 1.8f;
        
        // Render
        glBegin(GL_LINES);
        float posOffset = 0.0f;//120.0f;
        
        glUniformRotation(rotationX, rotationY, rotationZ);
        glUniformTranslate(-posOffset, 0.0f, 0.0f);
        glDrawBuffer(0, sizeof(vertexBuffer) / 10);
        
        //glUniformRotation(-rotationX, -rotationY, -rotationZ);
        //glUniformTranslate(posOffset, 0.0f, 0.0f);
        //glDrawBuffer(0, sizeof(vertexBuffer) / 10);
        
        SwapBuffers();
        glClear(0);
    }
    
    gl_init(0);
}

#endif
