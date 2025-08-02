require "test_helper"

class ObjectIdentityTest < Minitest::Test
  def test_separate_identical_objects_remain_separate
    # This test ensures that separate Ruby objects with identical content
    # become separate JavaScript objects, not shared references
    context = MiniRacer::Context.new
    
    # Create three separate Ruby objects with identical content
    obj1 = { "value" => 42 }
    obj2 = { "value" => 42 }
    obj3 = { "value" => 42 }
    
    # Verify they are separate Ruby objects but content-equal
    assert_equal false, obj1.equal?(obj2), "Ruby objects should not be identical"
    assert_equal true, obj1 == obj2, "Ruby objects should be content-equal"
    
    context.attach("get_objects", proc { [obj1, obj2, obj3] })
    result = context.eval(<<~JS)
      (function() {
        const arr = get_objects();
        
        // Check JavaScript object identity
        const identity_checks = {
          obj0_eq_obj1: arr[0] === arr[1],
          obj0_eq_obj2: arr[0] === arr[2], 
          obj1_eq_obj2: arr[1] === arr[2]
        };
        
        // Mutate each object differently
        arr[0].mutated_id = "zero";
        arr[1].mutated_id = "one";
        arr[2].mutated_id = "two";
        
        return {
          identities: identity_checks,
          final_ids: [arr[0].mutated_id, arr[1].mutated_id, arr[2].mutated_id]
        };
      })();
    JS
    
    # Verify that JavaScript objects are NOT identical (separate objects)
    assert_equal false, result['identities']['obj0_eq_obj1'], 
                 "JS objects with identical content should NOT share identity"
    assert_equal false, result['identities']['obj0_eq_obj2'],
                 "JS objects with identical content should NOT share identity"
    assert_equal false, result['identities']['obj1_eq_obj2'],
                 "JS objects with identical content should NOT share identity"
    
    # Verify that mutations don't affect other objects (separate identity confirmed)
    assert_equal ["zero", "one", "two"], result['final_ids'],
                 "Each object should maintain separate mutations"
  end
  
  def test_same_ruby_object_multiple_times_shares_identity
    # This test ensures that the SAME Ruby object referenced multiple times
    # correctly shares JavaScript identity (for circular reference handling)
    context = MiniRacer::Context.new
    
    # Create one Ruby object and reference it multiple times
    shared_obj = { "value" => 42 }
    objects = [shared_obj, shared_obj, shared_obj]
    
    # Verify they are the same Ruby object
    assert_equal true, objects[0].equal?(objects[1]), "Should be same Ruby object"
    assert_equal true, objects[0].equal?(objects[2]), "Should be same Ruby object"
    
    context.attach("get_objects", proc { objects })
    result = context.eval(<<~JS)
      (function() {
        const arr = get_objects();
        
        // Check JavaScript object identity
        const identity_checks = {
          obj0_eq_obj1: arr[0] === arr[1],
          obj0_eq_obj2: arr[0] === arr[2],
          obj1_eq_obj2: arr[1] === arr[2]
        };
        
        // Mutate first object
        arr[0].mutated_id = "shared";
        
        return {
          identities: identity_checks,
          final_ids: [arr[0].mutated_id, arr[1].mutated_id, arr[2].mutated_id]
        };
      })();
    JS
    
    # Verify that JavaScript objects ARE identical (shared object)  
    assert_equal true, result['identities']['obj0_eq_obj1'],
                 "Same Ruby object should share JS identity" 
    assert_equal true, result['identities']['obj0_eq_obj2'],
                 "Same Ruby object should share JS identity"
    assert_equal true, result['identities']['obj1_eq_obj2'],
                 "Same Ruby object should share JS identity"
    
    # Verify that mutation affects all references (shared identity confirmed)
    assert_equal ["shared", "shared", "shared"], result['final_ids'],
                 "Shared object should show mutation across all references"
  end
  
  def test_nested_objects_with_identical_content
    # Test nested structures to ensure the fix works recursively
    context = MiniRacer::Context.new
    
    # Create nested objects with identical content but separate instances
    obj1 = { "outer" => { "inner" => { "value" => 42 } } }
    obj2 = { "outer" => { "inner" => { "value" => 42 } } }
    
    context.attach("get_objects", proc { [obj1, obj2] })
    result = context.eval(<<~JS)
      (function() {
        const arr = get_objects();
        
        // Mutate nested properties
        arr[0].outer.inner.id = "first";
        arr[1].outer.inner.id = "second";
        
        return [
          arr[0].outer.inner.id,
          arr[1].outer.inner.id,
          arr[0] === arr[1]
        ];
      })();
    JS
    
    assert_equal "first", result[0], "First nested object should maintain separate identity"
    assert_equal "second", result[1], "Second nested object should maintain separate identity"  
    assert_equal false, result[2], "Nested objects should not share identity"
  end
end