# Emit straight-line guest code as many small translation blocks, each ended by a jump.
import sys
name, blocks = sys.argv[1], int(sys.argv[2])
out = [".text", ".globl %s" % name, ".type %s,@function" % name, "%s:" % name]
for i in range(blocks):
    out.append(".L%s_%d:" % (name, i))
    out.append("  add $%d, %%rax" % (i % 251 + 1))
    out.append("  xor %rdx, %rax")
    out.append("  lea 3(%rax,%rdx), %rdx")
    out.append("  add %rax, %rdx")
    out.append("  ror $7, %rax")
    out.append("  sub %rdx, %rax")
    out.append("  imul $3, %rdx, %rdx")
    out.append("  jmp .L%s_%d" % (name, i + 1))
out.append(".L%s_%d:" % (name, blocks))
out.append("  ret")
out.append(".size %s, .-%s" % (name, name))
out.append('.section .note.GNU-stack,"",@progbits')
print("\n".join(out))
