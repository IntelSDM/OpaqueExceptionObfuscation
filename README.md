# Opaque Exception Obfuscation
Built for LLVM 19.1 - Windows x64 <br> <br>

Optimisers have gotten quite popular, But what if you can't trust your solver? What if the equation was rigged? Thats what we do here. <br>
By using the [stack negative](https://connorjaydunn.github.io/blog/posts/denuvo-analysis/) trick, we can effectively make equations that are true, but will always return false and vice versa. <br>
Okay so nanomite tricks right? No, we aren't redirecting, rewriting any values, we let windows do it all, the behaviour is all obscured by windows, we have nothing to do with it, we just tell the exception handler to continue. <br>
This means that Z3 and SMT solvers and general purposes optimisers can't be trusted, as they will falsely solve these, resulting in them breaking the application, forcing your analyst to filter them by hand, making custom rules for their automated tools <br>
For the sake of clarity, you are meant to mix these into your actual opaque predicates! Not by themselves, the idea is to make the optimsers untrustworthy as they won't account for windows overwriting the logic. <br>
Doing this by itself will have no effect on optimisers as they will see that it is always at its simplest form; and leave it. <br>

<img width="1049" height="566" alt="image" src="https://github.com/user-attachments/assets/9a7e31bb-0baa-4cec-b2d7-217846edb3b2" /> <br>
var_298= qword ptr -298h <br>
We load a value into RAX, then store it in [rsp+118h+var_298] then load the same value into RCX, Compare. They should be equal, nothing else happened... <br>
Except mov     byte ptr ds:0, 0 caused an access violation. [rsp+118h+var_298] is now some random value windows dropped into it. <br>

This is just another opaque technique to add into your opaque engine. <br>

## Annotations
```c
__attribute__((annotate("opqexc")))
``` 
* Applies opaque generation to your function

## How To Build
* Download [LLVM 19.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.0)
* Download [VCPKG](https://github.com/microsoft/vcpkg)
* VCPKG install asmjit
* Open build.bat in a text editor.
* Find the LLVM 19.1 references in the command line, Change them to your file path.
* Find the VCPKG references in the command line, Change them to your file path.
* Run the build script
* Build your application with the ``` -load-pass-plugin ``` Param to the FunctionPass.dll
